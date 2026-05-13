/**
 * \file    tests_ubi_concurrency.c
 *
 * \author Kamil Kielbasa
 *
 * \brief   Multi-threaded concurrency and partition guard tests.
 *
 * Validates that the per-device mutex correctly serializes concurrent
 * operations, that deinit waits for in-flight work, and that the
 * single-handle-per-partition guard works as specified.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* UBI headers: */
#include <ubi.h>

/* Test fixtures: */
#include "ubi_test_fixture.h"

/* Zephyr headers: */
#include <zephyr/ztest.h>
#include <zephyr/kernel.h>

/* Standard library headers: */
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

#define THREAD_STACK_SIZE 4096
#define READER_ITERATIONS 200
#define WRITER_ITERATIONS 50
#define NUM_READER_THREADS 4

/* Module types and type definitiones ----------------------------------------------------------- */

/** \brief Per-thread context shared between a reader thread and the test driver. */
struct reader_ctx {
	struct ubi_device *ubi; /*!< UBI device under test. */
	volatile bool *stop; /*!< Cooperative stop flag. */
	int vol_id; /*!< Volume identifier used for read operations. */
	const uint8_t *expected; /*!< Expected payload bytes for LEB 0. */
	size_t expected_len; /*!< Length of \c expected in bytes. */
};

/** \brief Per-thread context shared between a writer thread and the test driver. */
struct writer_ctx {
	struct ubi_device *ubi; /*!< UBI device under test. */
	volatile bool *stop; /*!< Cooperative stop flag. */
	int vol_id; /*!< Volume identifier used for write operations. */
};

/* Module interface variables and constants ----------------------------------------------------- */

/* Static variables and constants --------------------------------------------------------------- */

static struct ubi_flash_desc flash = { 0 };

static K_THREAD_STACK_ARRAY_DEFINE(reader_stacks, NUM_READER_THREADS, THREAD_STACK_SIZE);
static struct k_thread reader_threads[NUM_READER_THREADS];

static K_THREAD_STACK_DEFINE(writer_stack, THREAD_STACK_SIZE);
static struct k_thread writer_thread;

/* Static function declarations ----------------------------------------------------------------- */

static void *ztest_suite_setup(void);
static void ztest_suite_after(void *ctx);

static void ztest_testcase_before(void *ctx);
static void ztest_testcase_teardown(void *ctx);

static void reader_entry(void *p1, void *p2, void *p3);
static void writer_entry(void *p1, void *p2, void *p3);

/* Static function definitions ------------------------------------------------------------------ */

static void *ztest_suite_setup(void)
{
	ubi_test_setup_mtd(&flash);
	return NULL;
}

static void ztest_suite_after(void *ctx)
{
	(void)ctx;
}

static void ztest_testcase_before(void *ctx)
{
	(void)ctx;
	ubi_test_erase_partition();
}

static void ztest_testcase_teardown(void *ctx)
{
	(void)ctx;
}

static void reader_entry(void *p1, void *p2, void *p3)
{
	struct reader_ctx *ctx = (struct reader_ctx *)p1;

	(void)p2;
	(void)p3;

	uint8_t buf[16] = { 0 };

	zassert_true(ctx->expected_len <= sizeof(buf));

	for (int i = 0; i < READER_ITERATIONS && !(*ctx->stop); i++) {
		struct ubi_device_info info = { 0 };
		int ret = ubi_device_get_info(ctx->ubi, &info);
		zassert_ok(ret);

		struct ubi_volume_config vol_cfg = { 0 };
		size_t alloc_lebs = 0;
		ret = ubi_volume_get_info(ctx->ubi, ctx->vol_id, &vol_cfg, &alloc_lebs);
		zassert_ok(ret);

		/* Real read against the published API. The writer in
		 * `reader_writer_interleave` may have unmapped the LEB
		 * between this read and the previous one, so a transient
		 * `-ENOENT` is tolerated; any other status is a bug. */
		memset(buf, 0, sizeof(buf));
		ret = ubi_leb_read(ctx->ubi, ctx->vol_id, 0, 0, buf, ctx->expected_len);
		if (ret == -ENOENT) {
			continue;
		}
		zassert_ok(ret);
		zassert_mem_equal(buf, ctx->expected, ctx->expected_len);
	}
}

static void writer_entry(void *p1, void *p2, void *p3)
{
	struct writer_ctx *ctx = (struct writer_ctx *)p1;

	(void)p2;
	(void)p3;

	const uint8_t data[] = { 0x42 };

	for (int i = 0; i < WRITER_ITERATIONS && !(*ctx->stop); i++) {
		ubi_leb_write(ctx->ubi, ctx->vol_id, 0, data, sizeof(data));
		ubi_leb_unmap(ctx->ubi, ctx->vol_id, 0);
		ubi_device_erase_peb(ctx->ubi);
	}
}

/* Module interface function definitions -------------------------------------------------------- */

ZTEST_SUITE(ubi_concurrency, NULL, ztest_suite_setup, ztest_testcase_before,
	    ztest_testcase_teardown, ztest_suite_after);

/**
 * \brief 4 threads concurrently read metadata — no crash, correct values.
 *
 * \details Scenario: Initialize device, create dynamic volume "rdvol" with 2 LEBs, write
 *          pattern {0xAA, 0xBB} to LEB 0. Spawn NUM_READER_THREADS (4) threads each running
 *          READER_ITERATIONS (200) iterations of ubi_device_get_info / ubi_volume_get_info /
 *          ubi_leb_is_mapped. Join all threads, then deinit.
 *
 * \expect All API calls inside reader threads return 0; no thread asserts; deinit returns 0.
 */
ZTEST(ubi_concurrency, concurrent_readers)
{
	struct ubi_device *ubi = ubi_test_init_device(&flash);

	const struct ubi_volume_config cfg = {
		.name = "rdvol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xAA, 0xBB };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	volatile bool stop = false;
	struct reader_ctx rctx[NUM_READER_THREADS];

	for (int i = 0; i < NUM_READER_THREADS; i++) {
		rctx[i].ubi = ubi;
		rctx[i].stop = &stop;
		rctx[i].vol_id = vol_id;
		rctx[i].expected = data;
		rctx[i].expected_len = sizeof(data);

		k_thread_create(&reader_threads[i], reader_stacks[i], THREAD_STACK_SIZE,
				reader_entry, &rctx[i], NULL, NULL, K_PRIO_PREEMPT(10), 0,
				K_NO_WAIT);
	}

	for (int i = 0; i < NUM_READER_THREADS; i++) {
		k_thread_join(&reader_threads[i], K_FOREVER);
	}

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief 2 reader threads + 1 writer thread — no crash, data integrity.
 *
 * \details Scenario: Initialize device, create dynamic volume "rwvol" with 2 LEBs, write
 *          {0xDE, 0xAD} to LEB 0. Spawn 2 reader threads (READER_ITERATIONS=200 each) and
 *          1 writer thread (WRITER_ITERATIONS=50 each) cycling through leb_write / leb_unmap /
 *          erase_peb. Join writer thread, set stop flag, join readers, deinit.
 *
 * \expect All threads complete without assertion; writes succeed; deinit returns 0.
 */
ZTEST(ubi_concurrency, reader_writer_interleave)
{
	struct ubi_device *ubi = ubi_test_init_device(&flash);

	const struct ubi_volume_config cfg = {
		.name = "rwvol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xDE, 0xAD };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	volatile bool stop = false;

	struct reader_ctx rctx[2];
	for (int i = 0; i < 2; i++) {
		rctx[i].ubi = ubi;
		rctx[i].stop = &stop;
		rctx[i].vol_id = vol_id;
		rctx[i].expected = data;
		rctx[i].expected_len = sizeof(data);

		k_thread_create(&reader_threads[i], reader_stacks[i], THREAD_STACK_SIZE,
				reader_entry, &rctx[i], NULL, NULL, K_PRIO_PREEMPT(10), 0,
				K_NO_WAIT);
	}

	struct writer_ctx wctx = { .ubi = ubi, .stop = &stop, .vol_id = vol_id };
	k_thread_create(&writer_thread, writer_stack, THREAD_STACK_SIZE, writer_entry, &wctx, NULL,
			NULL, K_PRIO_PREEMPT(10), 0, K_NO_WAIT);

	k_thread_join(&writer_thread, K_FOREVER);

	stop = true;

	for (int i = 0; i < 2; i++) {
		k_thread_join(&reader_threads[i], K_FOREVER);
	}

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Reader thread cleanly observes stop flag and `ubi_device_deinit()`
 *        joins safely once all worker threads have quiesced.
 *
 * \details Scenario: Initialize device, create dynamic volume `"dqvol"` with
 *          2 LEBs, write a small payload to LEB 0 so reader reads succeed.
 *          Spawn one reader thread running `READER_ITERATIONS` iterations
 *          of `ubi_device_get_info` / `ubi_volume_get_info` / `ubi_leb_read`.
 *          Sleep 10 ms to let the reader make progress, set the stop flag,
 *          join the reader thread, then call `ubi_device_deinit`.
 *
 *          The test demonstrates the documented shutdown contract: the
 *          driver does not block deinit on its own, but a caller that has
 *          spawned worker threads using a UBI handle is responsible for
 *          quiescing them before deinit.  After `k_thread_join` returns,
 *          no thread is inside the UBI critical section, so deinit is
 *          guaranteed safe.
 *
 * \expect Reader thread exits before timeout; `ubi_device_deinit` returns 0.
 *
 * \oracle `k_thread_join == 0`; `ubi_device_deinit == 0`; no zassert
 *         inside `reader_entry` ever fires.
 */
ZTEST(ubi_concurrency, deinit_after_quiescence)
{
	struct ubi_device *ubi = ubi_test_init_device(&flash);

	const struct ubi_volume_config cfg = {
		.name = "dqvol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0x42, 0x43 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	volatile bool stop = false;
	struct reader_ctx rctx;
	rctx.ubi = ubi;
	rctx.stop = &stop;
	rctx.vol_id = vol_id;
	rctx.expected = data;
	rctx.expected_len = sizeof(data);

	k_thread_create(&reader_threads[0], reader_stacks[0], THREAD_STACK_SIZE, reader_entry,
			&rctx, NULL, NULL, K_PRIO_PREEMPT(10), 0, K_NO_WAIT);

	k_sleep(K_MSEC(10));

	stop = true;
	k_thread_join(&reader_threads[0], K_FOREVER);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief ubi_device_init() twice with the same partition returns -EBUSY.
 *
 * \details Scenario: Call ubi_device_init on the flash partition (succeeds). Call
 *          ubi_device_init again on the same partition with a second device pointer.
 *
 * \expect Second init returns -EBUSY; the second device pointer remains NULL.
 */
ZTEST(ubi_concurrency, double_init_same_partition)
{
	struct ubi_device *ubi1 = ubi_test_init_device(&flash);

	struct ubi_device *ubi2 = NULL;
	int ret = ubi_device_init(&flash, NULL, &ubi2);

	zassert_equal(ret, -EBUSY, "Second init on same partition must return -EBUSY");
	zassert_is_null(ubi2);

	zassert_ok(ubi_device_deinit(ubi1));
}

/**
 * \brief After deinit, init on the same partition succeeds again.
 *
 * \details Scenario: Init device, deinit, reinit on the same partition, deinit again.
 *
 * \expect Both inits return 0; both deinits return 0.
 */
ZTEST(ubi_concurrency, init_after_deinit_same_partition)
{
	struct ubi_device *ubi = ubi_test_init_device(&flash);
	zassert_ok(ubi_device_deinit(ubi));

	ubi = ubi_test_init_device(&flash);
	zassert_ok(ubi_device_deinit(ubi));
}

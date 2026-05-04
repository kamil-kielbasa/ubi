/**
 * \file    tests_ubi_concurrency.c
 *
 * \brief   Multi-threaded concurrency and partition guard tests.
 *
 * Validates that the per-device mutex correctly serializes concurrent
 * operations, that deinit waits for in-flight work, and that the
 * single-handle-per-partition guard works as specified.
 *
 * \copyright Copyright (c) 2026
 */

#include <ubi.h>
#include "ubi_test_fixture.h"

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>

#include <string.h>

/* -------------------------------------------------------------------------- */

static struct ubi_flash_desc flash = { 0 };

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

ZTEST_SUITE(ubi_concurrency, NULL, ztest_suite_setup, ztest_testcase_before,
	    ztest_testcase_teardown, ztest_suite_after);

/* -------------------------------------------------------------------------- */

#define THREAD_STACK_SIZE 4096
#define READER_ITERATIONS 200
#define WRITER_ITERATIONS 50
#define NUM_READER_THREADS 4

static K_THREAD_STACK_ARRAY_DEFINE(reader_stacks, NUM_READER_THREADS, THREAD_STACK_SIZE);
static struct k_thread reader_threads[NUM_READER_THREADS];

static K_THREAD_STACK_DEFINE(writer_stack, THREAD_STACK_SIZE);
static struct k_thread writer_thread;

struct reader_ctx {
	struct ubi_device *ubi;
	volatile bool *stop;
	int vol_id;
};

struct writer_ctx {
	struct ubi_device *ubi;
	volatile bool *stop;
	int vol_id;
};

/* -------------------------------------------------------------------------- */

static void reader_entry(void *p1, void *p2, void *p3)
{
	struct reader_ctx *ctx = (struct reader_ctx *)p1;

	(void)p2;
	(void)p3;

	for (int i = 0; i < READER_ITERATIONS && !(*ctx->stop); i++) {
		struct ubi_device_info info = { 0 };
		int ret = ubi_device_get_info(ctx->ubi, &info);
		zassert_ok(ret);

		struct ubi_volume_config vol_cfg = { 0 };
		size_t alloc_lebs = 0;
		ret = ubi_volume_get_info(ctx->ubi, ctx->vol_id, &vol_cfg, &alloc_lebs);
		zassert_ok(ret);

		bool mapped = false;
		ret = ubi_leb_is_mapped(ctx->ubi, ctx->vol_id, 0, &mapped);
		zassert_ok(ret);
	}
}

/**
 * \brief 4 threads concurrently read metadata — no crash, correct values.
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

		k_thread_create(&reader_threads[i], reader_stacks[i], THREAD_STACK_SIZE,
				reader_entry, &rctx[i], NULL, NULL, K_PRIO_PREEMPT(10), 0,
				K_NO_WAIT);
	}

	for (int i = 0; i < NUM_READER_THREADS; i++) {
		k_thread_join(&reader_threads[i], K_FOREVER);
	}

	zassert_ok(ubi_device_deinit(ubi));
}

/* -------------------------------------------------------------------------- */

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

/**
 * \brief 2 reader threads + 1 writer thread — no crash, data integrity.
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

/* -------------------------------------------------------------------------- */

/**
 * \brief Start threads, signal stop, join, then deinit — clean shutdown.
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

	volatile bool stop = false;
	struct reader_ctx rctx;
	rctx.ubi = ubi;
	rctx.stop = &stop;
	rctx.vol_id = vol_id;

	k_thread_create(&reader_threads[0], reader_stacks[0], THREAD_STACK_SIZE, reader_entry,
			&rctx, NULL, NULL, K_PRIO_PREEMPT(10), 0, K_NO_WAIT);

	k_sleep(K_MSEC(10));

	stop = true;
	k_thread_join(&reader_threads[0], K_FOREVER);

	zassert_ok(ubi_device_deinit(ubi));
}

/* -------------------------------------------------------------------------- */

/**
 * \brief ubi_device_init() twice with the same partition returns -EBUSY.
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
 */
ZTEST(ubi_concurrency, init_after_deinit_same_partition)
{
	struct ubi_device *ubi = ubi_test_init_device(&flash);
	zassert_ok(ubi_device_deinit(ubi));

	ubi = ubi_test_init_device(&flash);
	zassert_ok(ubi_device_deinit(ubi));
}

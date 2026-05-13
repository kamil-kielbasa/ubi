/**
 * \file    tests_ubi_secure_concurrency.c
 * \author  Kamil Kielbasa
 *
 * \brief   Multi-threaded concurrency parity tests for the secure backend.
 *
 * \details Mirrors the plain-mode `tests_ubi_concurrency.c` cases that
 *          exercise the per-device mutex under concurrent metadata reads
 *          and a single concurrent writer.  Asserts that secure
 *          backend's authenticated read / write paths serialise
 *          correctly so concurrent callers never observe a torn AEAD
 *          state or a stale cached counter.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* UBI headers: */
#include <ubi.h>
#include <ubi_crypto.h>
#include <ubi_test.h>

/* Test fixtures: */
#include "ubi_test_fixture.h"
#include "ubi_test_secure_fixture.h"

/* Zephyr headers: */
#include <psa/crypto.h>
#include <zephyr/ztest.h>
#include <zephyr/kernel.h>

/* Standard library headers: */
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

#define THREAD_STACK_SIZE 4096

/* Stack/thread arrays are sized for the worst-case test (`concurrent_readers`).
 * Other tests reuse the leading slots only. */
#define MAX_READER_THREADS 4U

/* Per-LEB pre-populated payload size; small enough to keep the suite cheap and
 * large enough that a torn AEAD read would surface as a `zassert_mem_equal`
 * mismatch instead of a single-byte coincidence. */
#define READ_VERIFY_PAYLOAD_LEN 4U

/* `concurrent_readers` parameters. */
#define CONCURRENT_READERS_NUM_THREADS MAX_READER_THREADS
#define CONCURRENT_READERS_VOLUME_LEBS MAX_READER_THREADS
#define CONCURRENT_READERS_ITERATIONS 100U

/* `reader_writer_interleave` parameters. */
#define READER_WRITER_VOLUME_LEBS 2U
#define READER_WRITER_NUM_READERS 2U
#define READER_WRITER_STABLE_LNUM 1U /*!< Reader-only LEB: writer never touches it. */
#define READER_WRITER_WRITER_LNUM 0U /*!< Writer-only LEB: read-only thread skips it. */
#define READER_WRITER_READER_ITERATIONS 100U
#define READER_WRITER_WRITER_ITERATIONS 25U

/* Module types and type definitiones ----------------------------------------------------------- */

/** \brief Per-thread context shared between a reader thread and the test driver. */
struct reader_ctx {
	struct ubi_device *ubi; /*!< UBI device under test. */
	volatile bool *stop; /*!< Cooperative stop flag. */
	int vol_id; /*!< Volume identifier used for read operations. */
	size_t verify_lnum; /*!< LEB whose payload the reader authenticates. */
	const uint8_t *expected_payload; /*!< Expected `READ_VERIFY_PAYLOAD_LEN` bytes. */
	size_t iterations; /*!< Number of read iterations to perform. */
};

/** \brief Per-thread context shared between a writer thread and the test driver. */
struct writer_ctx {
	struct ubi_device *ubi; /*!< UBI device under test. */
	volatile bool *stop; /*!< Cooperative stop flag. */
	int vol_id; /*!< Volume identifier used for write operations. */
	size_t writer_lnum; /*!< LEB the writer mutates. */
};

/* Module interface variables and constants ----------------------------------------------------- */

/* Static variables and constants --------------------------------------------------------------- */

static struct ubi_flash_desc flash = { 0 };

static K_THREAD_STACK_ARRAY_DEFINE(reader_stacks, MAX_READER_THREADS, THREAD_STACK_SIZE);
static struct k_thread reader_threads[MAX_READER_THREADS];

static K_THREAD_STACK_DEFINE(writer_stack, THREAD_STACK_SIZE);
static struct k_thread writer_thread;

/* Stable per-LEB payload pre-written before reader threads start.  Each byte
 * pair is unique per LEB index so a thread reading the wrong LEB (or a torn
 * AEAD record) would fail `zassert_mem_equal`. */
static const uint8_t lebs_payload[CONCURRENT_READERS_VOLUME_LEBS][READ_VERIFY_PAYLOAD_LEN] = {
	{ 0xA0, 0xA1, 0xA2, 0xA3 },
	{ 0xB0, 0xB1, 0xB2, 0xB3 },
	{ 0xC0, 0xC1, 0xC2, 0xC3 },
	{ 0xD0, 0xD1, 0xD2, 0xD3 },
};

/* Static function declarations ----------------------------------------------------------------- */

static void *ztest_suite_setup(void);
static void ztest_suite_before(void *ctx);

static void reader_entry(void *p1, void *p2, void *p3);
static void writer_entry(void *p1, void *p2, void *p3);

/* Static function definitions ------------------------------------------------------------------ */

static void *ztest_suite_setup(void)
{
	ubi_test_secure_suite_setup_impl(&flash);
	return NULL;
}

static void ztest_suite_before(void *ctx)
{
	(void)ctx;
	ubi_test_secure_before_impl();
}

static void reader_entry(void *p1, void *p2, void *p3)
{
	struct reader_ctx *ctx = (struct reader_ctx *)p1;

	(void)p2;
	(void)p3;

	for (size_t i = 0; i < ctx->iterations && !(*ctx->stop); i++) {
		struct ubi_device_info info = { 0 };
		int ret = ubi_device_get_info(ctx->ubi, &info);
		zassert_ok(ret);

		struct ubi_volume_config vol_cfg = { 0 };
		size_t alloc_lebs = 0;
		ret = ubi_volume_get_info(ctx->ubi, ctx->vol_id, &vol_cfg, &alloc_lebs);
		zassert_ok(ret);

		uint8_t buf[READ_VERIFY_PAYLOAD_LEN] = { 0 };
		ret = ubi_leb_read(ctx->ubi, ctx->vol_id, ctx->verify_lnum, 0, buf, sizeof(buf));
		zassert_ok(ret, "leb_read(lnum=%zu) failed: %d", ctx->verify_lnum, ret);
		zassert_mem_equal(buf, ctx->expected_payload, sizeof(buf),
				  "Torn AEAD read on lnum=%zu", ctx->verify_lnum);
	}
}

static void writer_entry(void *p1, void *p2, void *p3)
{
	struct writer_ctx *ctx = (struct writer_ctx *)p1;

	(void)p2;
	(void)p3;

	const uint8_t data[] = { 0x42 };

	for (size_t i = 0; i < READER_WRITER_WRITER_ITERATIONS && !(*ctx->stop); i++) {
		ubi_leb_write(ctx->ubi, ctx->vol_id, ctx->writer_lnum, data, sizeof(data));
		ubi_leb_unmap(ctx->ubi, ctx->vol_id, ctx->writer_lnum);
		ubi_device_erase_peb(ctx->ubi);
	}
}

/* Module interface function definitions -------------------------------------------------------- */

ZTEST_SUITE(ubi_secure_concurrency, NULL, ztest_suite_setup, ztest_suite_before, NULL, NULL);

/**
 * \brief 4 threads concurrently authenticate 4 distinct LEBs — no torn AEAD reads.
 *
 * \details Scenario: Initialise a secure device, create dynamic volume `"rdvol"` with
 *          `CONCURRENT_READERS_VOLUME_LEBS` LEBs and pre-populate every LEB with a
 *          unique `READ_VERIFY_PAYLOAD_LEN`-byte pattern (`0xA0..A3`, `0xB0..B3`, …).
 *          Spawn `CONCURRENT_READERS_NUM_THREADS` threads, each running
 *          `CONCURRENT_READERS_ITERATIONS` iterations of:
 *          `ubi_device_get_info` + `ubi_volume_get_info` + a full
 *          `ubi_leb_read` of *its own* LEB followed by `zassert_mem_equal` against
 *          the expected pattern.  Every read drives the secure backend's full VID
 *          lookup + AEAD authentication path under the per-device mutex; a torn
 *          record would surface either as `-EBADMSG` or as a `mem_equal` mismatch.
 *
 * \expect All API calls return 0; every read of every LEB matches its expected
 *         pattern across all `CONCURRENT_READERS_NUM_THREADS *
 *         CONCURRENT_READERS_ITERATIONS` reads; deinit returns 0.
 *
 * \oracle Across the full 4×100 = 400 authenticated reads, every per-thread
 *         `zassert_mem_equal(buf, lebs_payload[thread_id], 4)` holds and
 *         no API call returns `< 0`.
 *
 * \trace Plain parity → `tests_ubi_concurrency::concurrent_readers`.
 *
 * \precondition `CONFIG_FLASH_SIMULATOR` + `CONFIG_UBI_CRYPTO` +
 *               `CONFIG_MULTITHREADING`; four reader stacks of
 *               `THREAD_STACK_SIZE` bytes each.
 */
ZTEST(ubi_secure_concurrency, concurrent_readers)
{
	struct ubi_device *ubi = ubi_test_secure_init(&flash);

	const struct ubi_volume_config cfg = {
		.name = "rdvol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = CONCURRENT_READERS_VOLUME_LEBS,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	for (size_t lnum = 0; lnum < CONCURRENT_READERS_VOLUME_LEBS; lnum++) {
		zassert_ok(ubi_leb_write(ubi, vol_id, lnum, lebs_payload[lnum],
					 READ_VERIFY_PAYLOAD_LEN));
	}

	volatile bool stop = false;
	struct reader_ctx rctx[CONCURRENT_READERS_NUM_THREADS];

	for (size_t i = 0; i < CONCURRENT_READERS_NUM_THREADS; i++) {
		rctx[i].ubi = ubi;
		rctx[i].stop = &stop;
		rctx[i].vol_id = vol_id;
		rctx[i].verify_lnum = i;
		rctx[i].expected_payload = lebs_payload[i];
		rctx[i].iterations = CONCURRENT_READERS_ITERATIONS;

		k_thread_create(&reader_threads[i], reader_stacks[i], THREAD_STACK_SIZE,
				reader_entry, &rctx[i], NULL, NULL, K_PRIO_PREEMPT(10), 0,
				K_NO_WAIT);
	}

	for (size_t i = 0; i < CONCURRENT_READERS_NUM_THREADS; i++) {
		k_thread_join(&reader_threads[i], K_FOREVER);
	}

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief 2 readers verify a stable LEB while a writer mutates a sibling LEB.
 *
 * \details Scenario: Initialise a secure device, create dynamic volume `"rwvol"` with
 *          `READER_WRITER_VOLUME_LEBS` LEBs, pre-populate the writer's LEB
 *          (`READER_WRITER_WRITER_LNUM`) with a throw-away pattern and the readers'
 *          LEB (`READER_WRITER_STABLE_LNUM`) with the expected verification payload.
 *          Spawn `READER_WRITER_NUM_READERS` reader threads — each runs
 *          `READER_WRITER_READER_ITERATIONS` full authenticated reads of the stable
 *          LEB and asserts byte-for-byte equality with the expected payload.  Spawn
 *          one writer thread that cycles `leb_write` / `leb_unmap` / `erase_peb` on
 *          the writer's LEB for `READER_WRITER_WRITER_ITERATIONS` iterations,
 *          forcing anchor migrations and AEAD-counter bumps under the per-device
 *          mutex.  The mutex must serialise writer mutations with reader VID
 *          lookups so the readers' stable-LEB AEAD record is never observed torn.
 *
 * \expect All threads complete without assertion; every reader iteration's
 *         `ubi_leb_read` of the stable LEB returns 0 and matches the expected
 *         payload bit-exact; deinit returns 0.
 *
 * \oracle Across `READER_WRITER_NUM_READERS * READER_WRITER_READER_ITERATIONS`
 *         (2×100 = 200) reads of the stable LEB every
 *         `zassert_mem_equal(buf, stable_payload, 4)` holds while the writer
 *         completes `READER_WRITER_WRITER_ITERATIONS` mutation cycles in
 *         parallel.
 *
 * \trace Plain parity → `tests_ubi_concurrency::reader_writer_interleave`.
 *
 * \precondition `CONFIG_FLASH_SIMULATOR` + `CONFIG_UBI_CRYPTO` +
 *               `CONFIG_MULTITHREADING`; per-device mutex serialises
 *               mutators with reader VID lookups.
 */
ZTEST(ubi_secure_concurrency, reader_writer_interleave)
{
	struct ubi_device *ubi = ubi_test_secure_init(&flash);

	const struct ubi_volume_config cfg = {
		.name = "rwvol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = READER_WRITER_VOLUME_LEBS,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t writer_seed[READ_VERIFY_PAYLOAD_LEN] = { 0xDE, 0xAD, 0xBE, 0xEF };
	const uint8_t stable_payload[READ_VERIFY_PAYLOAD_LEN] = { 0x11, 0x22, 0x33, 0x44 };

	zassert_ok(ubi_leb_write(ubi, vol_id, READER_WRITER_WRITER_LNUM, writer_seed,
				 sizeof(writer_seed)));
	zassert_ok(ubi_leb_write(ubi, vol_id, READER_WRITER_STABLE_LNUM, stable_payload,
				 sizeof(stable_payload)));

	volatile bool stop = false;

	struct reader_ctx rctx[READER_WRITER_NUM_READERS];
	for (size_t i = 0; i < READER_WRITER_NUM_READERS; i++) {
		rctx[i].ubi = ubi;
		rctx[i].stop = &stop;
		rctx[i].vol_id = vol_id;
		rctx[i].verify_lnum = READER_WRITER_STABLE_LNUM;
		rctx[i].expected_payload = stable_payload;
		rctx[i].iterations = READER_WRITER_READER_ITERATIONS;

		k_thread_create(&reader_threads[i], reader_stacks[i], THREAD_STACK_SIZE,
				reader_entry, &rctx[i], NULL, NULL, K_PRIO_PREEMPT(10), 0,
				K_NO_WAIT);
	}

	struct writer_ctx wctx = {
		.ubi = ubi,
		.stop = &stop,
		.vol_id = vol_id,
		.writer_lnum = READER_WRITER_WRITER_LNUM,
	};
	k_thread_create(&writer_thread, writer_stack, THREAD_STACK_SIZE, writer_entry, &wctx, NULL,
			NULL, K_PRIO_PREEMPT(10), 0, K_NO_WAIT);

	k_thread_join(&writer_thread, K_FOREVER);

	stop = true;

	for (size_t i = 0; i < READER_WRITER_NUM_READERS; i++) {
		k_thread_join(&reader_threads[i], K_FOREVER);
	}

	zassert_ok(ubi_device_deinit(ubi));
}

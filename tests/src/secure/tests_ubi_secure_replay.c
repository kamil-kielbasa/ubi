/**
 * \file    tests_ubi_secure_replay.c
 * \author  Kamil Kielbasa
 *
 * \brief   Secure-specific tests: replay of authentic records to a
 *          different physical location (parent-child AAD binding).
 *
 * \details The secure on-flash format binds every authenticated record
 *          to its physical PEB index (and intra-PEB offset for LEB
 *          records) via the AEAD AAD.  Verbatim relocation of an
 *          authentic EC, VID, or LEB record from PEB A to PEB B must
 *          therefore fail authentication when the verifier rebuilds the
 *          AAD from PEB B's location.  Closes audit §10.1 #3
 *          (parent-child AAD binding test).
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
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>

/* Standard library headers: */
#include <errno.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

/* Module types and type definitiones ----------------------------------------------------------- */

/* Module interface variables and constants ----------------------------------------------------- */
#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/* Reserved PEBs occupy the first NR_OF_RES_PEBS slots; data PEBs follow. */
#define NR_OF_RES_PEBS (2U)

/* On-flash secure record layout per data PEB (matches lib/src/secure/ubi_secure_types.h). */
#define EC_REGION_OFFSET (0U)
#define EC_REGION_SIZE (64U)
#define VID_REGION_OFFSET (EC_REGION_SIZE)
#define VID_REGION_SIZE (96U)
#define LEB_REGION_OFFSET (EC_REGION_SIZE + VID_REGION_SIZE)

/* Big-endian 'UBIS' magic prefix bytes (sys_put_be32(0x55424953)). */
static const uint8_t UBIS_MAGIC_BE[4] = { 'U', 'B', 'I', 'S' };

/* Static variables and constants --------------------------------------------------------------- */

/* Static function declarations ----------------------------------------------------------------- */
static struct ubi_flash_desc flash = { 0 };
static struct ubi_device *g_ubi;
static size_t g_auth_failure_count;

/* Static function definitions ------------------------------------------------------------------ */
static enum ubi_crypto_event_verdict counting_event_cb(const struct ubi_crypto_event *event,
						       void *user_data)
{
	(void)user_data;
	if (event->type == UBI_CRYPTO_EVENT_AUTH_FAILURE) {
		g_auth_failure_count++;
	}
	return UBI_CRYPTO_EVENT_CONTINUE;
}

static void *ztest_suite_setup(void)
{
	ubi_test_secure_suite_setup_impl(&flash);
	return NULL;
}

static void ztest_suite_before(void *ctx)
{
	(void)ctx;
	ubi_test_secure_before_impl();
	g_ubi = NULL;
	g_auth_failure_count = 0;
}

static void ztest_suite_after(void *ctx)
{
	(void)ctx;
	if (g_ubi) {
		(void)ubi_device_deinit(g_ubi);
		g_ubi = NULL;
	}
}

/**
 * \brief Find data PEBs that hold an authentic VID record (i.e. mapped LEBs).
 *
 * \details Scans data PEBs **in ascending pnum order** and returns those
 *          whose VID region begins with the 'UBIS' big-endian magic.
 *          Reserved PEBs are skipped.  Combined with the secure backend's
 *          deterministic highest-free-pnum allocation strategy, this lets
 *          tests deduce the lnum-to-pnum mapping from write order: the
 *          first written LEB lands on the **highest** mapped pnum, the
 *          second on the next-lower one, and so on.  Concretely, with
 *          two LEBs written in order (lnum 0, lnum 1), the returned
 *          \c mapped[0] (lower pnum) holds lnum 1 and \c mapped[1]
 *          (higher pnum) holds lnum 0.
 *
 * \param[out] out_pnums  Receives PEB indices (must hold at least \p max).
 * \param[in]  max        Maximum number of indices to fill.
 *
 * \return Number of mapped data PEBs found (clamped to \p max).
 */
static size_t find_mapped_data_pebs(size_t *out_pnums, size_t max)
{
	const struct device *dev = UBI_PARTITION_DEVICE;
	const size_t ebs = flash.erase_block_size;
	const size_t nr_blocks = UBI_PARTITION_SIZE / ebs;
	size_t found = 0;

	for (size_t pnum = NR_OF_RES_PEBS; pnum < nr_blocks && found < max; pnum++) {
		uint8_t magic[4] = { 0 };
		const off_t off =
			UBI_PARTITION_OFFSET + (off_t)(pnum * ebs) + (off_t)VID_REGION_OFFSET;
		zassert_ok(flash_read(dev, off, magic, sizeof(magic)));
		if (memcmp(magic, UBIS_MAGIC_BE, sizeof(magic)) == 0) {
			out_pnums[found++] = pnum;
		}
	}

	return found;
}

/**
 * \brief Copy a sub-region from PEB \p src_pnum into PEB \p dst_pnum (verbatim).
 *
 * \details Reads the entire erase block of \p dst_pnum, overlays
 *          [\p region_off, \p region_off + \p region_len) with the
 *          corresponding bytes from \p src_pnum, then erases and rewrites
 *          \p dst_pnum.  Used to forge a "replay-to-other-location" attack:
 *          PEB \p dst_pnum on flash now bears an authentic record produced
 *          for PEB \p src_pnum, so the verifier (which derives AAD from
 *          \p dst_pnum) must reject it.
 */
static void copy_region_between_pebs(size_t src_pnum, size_t dst_pnum, size_t region_off,
				     size_t region_len)
{
	const struct device *dev = UBI_PARTITION_DEVICE;
	const size_t ebs = flash.erase_block_size;

	zassert_true(region_off + region_len <= ebs);
	zassert_not_equal(src_pnum, dst_pnum);

	uint8_t *dst_block = k_malloc(ebs);
	uint8_t *src_region = k_malloc(region_len);
	zassert_not_null(dst_block);
	zassert_not_null(src_region);

	/* Read destination block (whole erase block) and source region. */
	zassert_ok(flash_read(dev, UBI_PARTITION_OFFSET + (off_t)(dst_pnum * ebs), dst_block, ebs));
	zassert_ok(flash_read(dev,
			      UBI_PARTITION_OFFSET + (off_t)(src_pnum * ebs) + (off_t)region_off,
			      src_region, region_len));

	/* Overlay and write back. */
	memcpy(&dst_block[region_off], src_region, region_len);
	zassert_ok(flash_erase(dev, UBI_PARTITION_OFFSET + (off_t)(dst_pnum * ebs), ebs));
	zassert_ok(
		flash_write(dev, UBI_PARTITION_OFFSET + (off_t)(dst_pnum * ebs), dst_block, ebs));

	k_free(src_region);
	k_free(dst_block);
}

/**
 * \brief Format the device, create one volume, and write two LEBs.
 *
 * \param[in]  cfg       Crypto config.
 * \param[out] vol_id    Receives the created volume id.
 * \param[in]  payload0  Payload for lnum=0.
 * \param[in]  payload1  Payload for lnum=1.
 * \param[in]  payload_len  Bytes per LEB payload.
 */
static void setup_two_leb_device(struct ubi_crypto_config *cfg, int *vol_id,
				 const uint8_t *payload0, const uint8_t *payload1,
				 size_t payload_len)
{
	zassert_ok(ubi_device_init(&flash, cfg, &g_ubi));

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'r', 'p', 'l', 'y' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, vol_id));

	zassert_ok(ubi_leb_write(g_ubi, *vol_id, 0, payload0, payload_len));
	zassert_ok(ubi_leb_write(g_ubi, *vol_id, 1, payload1, payload_len));

	/* Sanity readback before tampering. */
	uint8_t rb[16] = { 0 };
	zassert_true(payload_len <= sizeof(rb));
	zassert_ok(ubi_leb_read(g_ubi, *vol_id, 0, 0, rb, payload_len));
	zassert_mem_equal(rb, payload0, payload_len);
	memset(rb, 0, sizeof(rb));
	zassert_ok(ubi_leb_read(g_ubi, *vol_id, 1, 0, rb, payload_len));
	zassert_mem_equal(rb, payload1, payload_len);

	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;
}

/* Module interface function definitions -------------------------------------------------------- */

ZTEST_SUITE(ubi_secure_replay, NULL, ztest_suite_setup, ztest_suite_before, ztest_suite_after,
	    NULL);

/**
 * \brief Replay an authentic EC record from one PEB to another.
 *
 * \details Scenario: EC AAD binds (prefix32, peb_index, offset).  Copying PEB A's
 *          full EC region (offset 0..64) verbatim onto PEB B yields a
 *          flash image where PEB B carries a record whose AAD references
 *          PEB A.  On reattach, the EC verifier rebuilds the AAD from
 *          PEB B's actual index and the AEAD tag check fails.  The init
 *          scan classifies PEB B as bad and drops the LEB previously
 *          mapped at PEB B from the EBA.
 *
 *          Mapping invariant (deterministic allocator + ascending pnum
 *          scan): the highest-free-pnum allocator places lnum 0 at
 *          \c mapped[1] and lnum 1 at \c mapped[0].  We replay
 *          mapped[0]→mapped[1], so the loss is on lnum 0 only.
 *
 *          Init-scan EC/VID auth failures are handled by
 *          \c ubi_move_to_bad_blocks() and do **not** raise a
 *          \c AUTH_FAILURE event (that channel is reserved for the read
 *          path); the subsequent read of lnum 0 returns an error because
 *          its EBA mapping is gone, so no event is expected here either.
 *
 * \expect
 *  - Reattach succeeds (init never aborts on per-PEB AUTH faults).
 *  - \c bad_peb_count >= 1.
 *  - lnum 1 still reads correctly with the original payload.
 *  - lnum 0 read fails.
 *  - No \c AUTH_FAILURE event is emitted (init-scan path).
 */
ZTEST(ubi_secure_replay, replay_ec_record_to_other_peb_rejected)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	cfg.event_cb = counting_event_cb;

	const uint8_t p0[] = { 0xAA, 0xBB, 0xCC, 0xDD };
	const uint8_t p1[] = { 0x11, 0x22, 0x33, 0x44 };
	int vol_id = -1;

	setup_two_leb_device(&cfg, &vol_id, p0, p1, sizeof(p0));

	/* Locate two mapped data PEBs and replay PEB A's EC onto PEB B. */
	size_t mapped[2] = { 0 };
	const size_t n = find_mapped_data_pebs(mapped, ARRAY_SIZE(mapped));
	zassert_equal(n, 2, "Expected exactly two mapped data PEBs, got %zu", n);

	copy_region_between_pebs(mapped[0], mapped[1], EC_REGION_OFFSET, EC_REGION_SIZE);

	/* Reattach must complete; PEB B (== lnum 1's PEB) must be bad. */
	g_auth_failure_count = 0;
	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(g_ubi, &info));
	zassert_true(info.bad_peb_count >= 1,
		     "Expected at least one bad PEB after EC replay, got %zu", info.bad_peb_count);

	/* Mapping invariant: highest-free-pnum allocator → lnum 0 lives at
	 * mapped[1] (higher pnum) and lnum 1 lives at mapped[0] (lower pnum).
	 * We replay mapped[0]→mapped[1], so the tampered PEB is lnum 0's. */
	uint8_t rb0[sizeof(p0)] = { 0 };
	uint8_t rb1[sizeof(p1)] = { 0 };
	const int r0 = ubi_leb_read(g_ubi, vol_id, 0, 0, rb0, sizeof(p0));
	const int r1 = ubi_leb_read(g_ubi, vol_id, 1, 0, rb1, sizeof(p1));
	zassert_not_equal(r0, 0, "lnum 0 (EC-tampered PEB) read must fail");
	zassert_ok(r1, "lnum 1 (untampered PEB) must still read, got %d", r1);
	zassert_mem_equal(rb1, p1, sizeof(p1));

	/* Init scan handles EC auth fail via bad-block migration, not via
	 * the read-path event channel — no AUTH_FAILURE expected. */
	zassert_equal(g_auth_failure_count, 0,
		      "EC replay must not emit AUTH_FAILURE (init-scan path), got %zu",
		      g_auth_failure_count);

	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;
}

/**
 * \brief Replay an authentic VID record from one PEB to another.
 *
 * \details Scenario: VID AAD binds (prefix32, peb_index, offset, ec, parent_ec_kv).
 *          Copying PEB A's VID region (offset 64..160) verbatim onto PEB B
 *          (leaving B's own EC and LEB intact) creates an image where the
 *          VID carries an AAD that references PEB A.  On reattach, the VID
 *          verifier reconstructs AAD from PEB B's index and the tag check
 *          fails.  The init scan marks PEB B as bad and drops the LEB
 *          mapped at PEB B from the EBA.
 *
 *          Mapping invariant: lnum 0 == mapped[1], lnum 1 == mapped[0].
 *          We replay mapped[0]→mapped[1] so lnum 0 is the lost one.
 *
 * \expect
 *  - Reattach succeeds.
 *  - \c bad_peb_count >= 1.
 *  - lnum 1 still reads correctly with the original payload.
 *  - lnum 0 read fails.
 *  - No \c AUTH_FAILURE event is emitted (init-scan path, no read fault).
 */
ZTEST(ubi_secure_replay, replay_vid_record_to_other_peb_rejected)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	cfg.event_cb = counting_event_cb;

	const uint8_t p0[] = { 0xCA, 0xFE, 0xBA, 0xBE };
	const uint8_t p1[] = { 0xDE, 0xAD, 0xBE, 0xEF };
	int vol_id = -1;

	setup_two_leb_device(&cfg, &vol_id, p0, p1, sizeof(p0));

	size_t mapped[2] = { 0 };
	const size_t n = find_mapped_data_pebs(mapped, ARRAY_SIZE(mapped));
	zassert_equal(n, 2, "Expected exactly two mapped data PEBs, got %zu", n);

	copy_region_between_pebs(mapped[0], mapped[1], VID_REGION_OFFSET, VID_REGION_SIZE);

	g_auth_failure_count = 0;
	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(g_ubi, &info));
	zassert_true(info.bad_peb_count >= 1,
		     "Expected at least one bad PEB after VID replay, got %zu", info.bad_peb_count);

	uint8_t rb0[sizeof(p0)] = { 0 };
	uint8_t rb1[sizeof(p1)] = { 0 };
	const int r0 = ubi_leb_read(g_ubi, vol_id, 0, 0, rb0, sizeof(p0));
	const int r1 = ubi_leb_read(g_ubi, vol_id, 1, 0, rb1, sizeof(p1));
	zassert_not_equal(r0, 0, "lnum 0 (VID-tampered PEB) read must fail");
	zassert_ok(r1, "lnum 1 (untampered PEB) must still read, got %d", r1);
	zassert_mem_equal(rb1, p1, sizeof(p1));

	zassert_equal(g_auth_failure_count, 0,
		      "VID replay must not emit AUTH_FAILURE (init-scan path), got %zu",
		      g_auth_failure_count);

	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;
}

/**
 * \brief Replay an authentic LEB record from one PEB to another.
 *
 * \details Scenario: LEB single-tag AAD binds (prefix32, peb_index, leb_offset, ec,
 *          parent_ec_kv, vol_id, lnum, sqnum, data_size, parent_vid_kv).
 *          Copying PEB A's LEB region (offset 160..end-of-PEB) verbatim
 *          onto PEB B leaves PEB B's authentic EC and VID untouched, but
 *          the relocated LEB carries a tag computed against PEB A's index
 *          (and against lnum 1 — see invariant below).  On the next read
 *          of lnum 0, the LEB read path rebuilds AAD with PEB B's index
 *          and lnum 0, the AEAD tag check fails with \c -EBADMSG, and an
 *          \c AUTH_FAILURE event is emitted.
 *
 *          Mapping invariant: lnum 0 == mapped[1], lnum 1 == mapped[0].
 *          We replay mapped[0]→mapped[1], so the failing read is lnum 0.
 *          lnum 1 is untouched and must still read correctly.
 *
 * \expect
 *  - Reattach succeeds (EC and VID still authenticate at PEB B).
 *  - lnum 1 reads correctly with the original payload, no event.
 *  - lnum 0 read returns a non-zero error and raises exactly one
 *    \c AUTH_FAILURE event.
 */
ZTEST(ubi_secure_replay, replay_leb_record_to_other_peb_rejected)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	cfg.event_cb = counting_event_cb;

	const uint8_t p0[] = { 0x01, 0x02, 0x03, 0x04 };
	const uint8_t p1[] = { 0x05, 0x06, 0x07, 0x08 };
	int vol_id = -1;

	setup_two_leb_device(&cfg, &vol_id, p0, p1, sizeof(p0));

	size_t mapped[2] = { 0 };
	const size_t n = find_mapped_data_pebs(mapped, ARRAY_SIZE(mapped));
	zassert_equal(n, 2, "Expected exactly two mapped data PEBs, got %zu", n);

	const size_t leb_region_len = flash.erase_block_size - LEB_REGION_OFFSET;
	copy_region_between_pebs(mapped[0], mapped[1], LEB_REGION_OFFSET, leb_region_len);

	/* Reattach: EC and VID still bind PEB B correctly, init scan succeeds
	 * and both LEB mappings remain in the EBA. */
	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));

	g_auth_failure_count = 0;

	/* lnum 1 (mapped[0], untampered) must read its original payload
	 * without raising an event. */
	uint8_t rb1[sizeof(p1)] = { 0 };
	const int r1 = ubi_leb_read(g_ubi, vol_id, 1, 0, rb1, sizeof(p1));
	zassert_ok(r1, "lnum 1 (untampered PEB) must still read, got %d", r1);
	zassert_mem_equal(rb1, p1, sizeof(p1));
	zassert_equal(g_auth_failure_count, 0,
		      "Read of untampered lnum 1 must not raise AUTH_FAILURE, got %zu",
		      g_auth_failure_count);

	/* lnum 0 (mapped[1], LEB tampered) must fail authentication and
	 * raise exactly one AUTH_FAILURE event. */
	uint8_t rb0[sizeof(p0)] = { 0 };
	const int r0 = ubi_leb_read(g_ubi, vol_id, 0, 0, rb0, sizeof(p0));
	zassert_not_equal(r0, 0, "lnum 0 (LEB-tampered PEB) read must fail");
	zassert_equal(g_auth_failure_count, 1,
		      "Expected exactly one AUTH_FAILURE event on lnum 0 read, got %zu",
		      g_auth_failure_count);

	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;
}

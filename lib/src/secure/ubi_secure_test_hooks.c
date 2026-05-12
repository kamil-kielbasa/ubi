/**
 * \file    ubi_secure_test_hooks.c
 * \author  Kamil Kielbasa
 * \brief   Secure backend test hooks — fault injection for crypto operations.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_secure_test_hooks.h"

#if defined(CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION)

#include "ubi_internal.h"
#include "ubi_secure_io.h"
#include "ubi_cache.h"

/* Standard library headers: */
#include <errno.h>
#include <stddef.h>
#include <string.h>

/* Static variables ----------------------------------------------------------------------------- */

static bool hooks_armed[UBI_SECURE_HOOK_COUNT];
static uint64_t leb_write_counter_floor;

/* Public function definitions ------------------------------------------------------------------ */

void ubi_secure_test_hook_reset(void)
{
	memset(hooks_armed, 0, sizeof(hooks_armed));
}

void ubi_secure_test_hook_set(enum ubi_secure_test_hook_stage stage, bool armed)
{
	if (stage < UBI_SECURE_HOOK_COUNT) {
		hooks_armed[stage] = armed;
	}
}

bool ubi_secure_test_hook_check(enum ubi_secure_test_hook_stage stage)
{
	if (stage < UBI_SECURE_HOOK_COUNT && hooks_armed[stage]) {
		hooks_armed[stage] = false;
		return true;
	}
	return false;
}

void ubi_secure_test_set_metadata_counters(struct ubi_device *ubi, uint64_t dev_hdr, uint64_t ec,
					   uint64_t vid)
{
	if (ubi == NULL) {
		return;
	}
	ubi->aead.next_res_peb = dev_hdr;
	ubi->aead.next_ec = ec;
	ubi->aead.next_vid = vid;
}

void ubi_secure_test_get_metadata_counters(const struct ubi_device *ubi, uint64_t *dev_hdr,
					   uint64_t *ec, uint64_t *vid)
{
	if (ubi == NULL) {
		return;
	}
	if (dev_hdr != NULL) {
		*dev_hdr = ubi->aead.next_res_peb;
	}
	if (ec != NULL) {
		*ec = ubi->aead.next_ec;
	}
	if (vid != NULL) {
		*vid = ubi->aead.next_vid;
	}
}

void ubi_secure_test_set_leb_write_counter_floor(uint64_t floor)
{
	leb_write_counter_floor = floor;
}

uint64_t ubi_secure_test_get_leb_write_counter_floor(void)
{
	return leb_write_counter_floor;
}

int ubi_secure_test_get_volume_cached_counter(struct ubi_device *ubi, int vol_id,
					      uint64_t *write_counter, uint64_t *total_auth_bytes)
{
	if (ubi == NULL) {
		return -EINVAL;
	}

	struct ubi_volume *vol = ubi_find_volume(ubi, vol_id);

	if (vol == NULL) {
		return -ENOENT;
	}

	if (write_counter != NULL) {
		*write_counter = vol->cached_leb_write_counter;
	}
	if (total_auth_bytes != NULL) {
		*total_auth_bytes = vol->cached_leb_total_auth_bytes;
	}
	return 0;
}

int ubi_secure_test_get_peb_for_lnum(struct ubi_device *ubi, int vol_id, size_t lnum,
				     size_t *out_pnum)
{
	if (ubi == NULL || out_pnum == NULL) {
		return -EINVAL;
	}

	struct ubi_volume *vol = ubi_find_volume(ubi, vol_id);

	if (vol == NULL) {
		return -ENOENT;
	}

	if (lnum == SIZE_MAX) {
		/* Sentinel: caller wants the hidden anchor PEB for this volume. */
		if (vol->anchor_pnum == SIZE_MAX) {
			return -ENOENT;
		}
		*out_pnum = vol->anchor_pnum;
		return 0;
	}

	const struct ubi_rbt_item *entry =
		ubi_cache_search((struct rbtree *)&vol->eba_tbl, (uint32_t)lnum);

	if (entry == NULL) {
		return -ENOENT;
	}

	*out_pnum = entry->value.pnum;
	return 0;
}

int ubi_secure_test_read_vid_meta_from_peb(struct ubi_device *ubi, size_t pnum,
					   uint64_t *write_counter, uint64_t *total_auth_bytes,
					   uint64_t *sqnum)
{
	if (ubi == NULL) {
		return -EINVAL;
	}

	struct ubi_ec_hdr ec_hdr = { 0 };
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };

	int ret = ubi_secure_ec_hdr_read(&ubi->flash, ubi->crypto_cfg, pnum, &ec_hdr, &ec_ctx);

	if (ret != 0) {
		return ret;
	}

	struct ubi_vid_hdr vid_hdr = { 0 };
	struct ubi_vid_secure_meta vid_meta = { 0 };
	struct ubi_secure_vid_auth_ctx vid_ctx = { 0 };

	ret = ubi_secure_vid_hdr_read(&ubi->flash, ubi->crypto_cfg, pnum, &ec_ctx, &vid_hdr,
				      &vid_meta, &vid_ctx);
	if (ret != 0) {
		return ret;
	}

	if (write_counter != NULL) {
		*write_counter = vid_meta.leb_write_counter;
	}
	if (total_auth_bytes != NULL) {
		*total_auth_bytes = vid_meta.leb_total_auth_bytes;
	}
	if (sqnum != NULL) {
		*sqnum = vid_hdr.sqnum;
	}
	return 0;
}

#endif /* CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION */

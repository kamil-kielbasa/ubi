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

/* Standard library headers: */
#include <errno.h>
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
	ubi->next_dev_hdr_counter = dev_hdr;
	ubi->next_ec_counter = ec;
	ubi->next_vid_counter = vid;
}

void ubi_secure_test_get_metadata_counters(const struct ubi_device *ubi, uint64_t *dev_hdr,
					   uint64_t *ec, uint64_t *vid)
{
	if (ubi == NULL) {
		return;
	}
	if (dev_hdr != NULL) {
		*dev_hdr = ubi->next_dev_hdr_counter;
	}
	if (ec != NULL) {
		*ec = ubi->next_ec_counter;
	}
	if (vid != NULL) {
		*vid = ubi->next_vid_counter;
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

#endif /* CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION */

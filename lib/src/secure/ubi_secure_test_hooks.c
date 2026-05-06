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
#include <string.h>

/* Static variables ----------------------------------------------------------------------------- */

static bool hooks_armed[UBI_SECURE_HOOK_COUNT];

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

#endif /* CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION */

/**
 * \file    ubi_secure_test_hooks.c
 * \brief   Secure backend test hooks — fault injection for crypto operations.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files ------------------------------------------------------------------------------- */
#include "ubi_secure_test_hooks.h"

#if defined(CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION)

#include <string.h>

/* Static variables ---------------------------------------------------------------------------- */

static bool hooks_armed[UBI_SECURE_HOOK_COUNT];

/* Public function definitions ----------------------------------------------------------------- */

void ubi_secure_test_hook_reset(void)
{
	memset(hooks_armed, 0, sizeof(hooks_armed));
}

void ubi_secure_test_hook_set(enum ubi_secure_test_hook_stage stage, bool armed)
{
	if ((int)stage >= 0 && stage < UBI_SECURE_HOOK_COUNT) {
		hooks_armed[stage] = armed;
	}
}

bool ubi_secure_test_hook_check(enum ubi_secure_test_hook_stage stage)
{
	if ((int)stage >= 0 && stage < UBI_SECURE_HOOK_COUNT && hooks_armed[stage]) {
		hooks_armed[stage] = false;
		return true;
	}
	return false;
}

#endif /* CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION */

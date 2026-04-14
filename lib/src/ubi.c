/**
 * \file    ubi.c
 * \brief   UBI public facade — runtime backend dispatch.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files ------------------------------------------------------------------------------- */

#include "ubi.h"
#include "ubi_backend.h"

#include <zephyr/logging/log.h>

#include <errno.h>

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Module interface function definitions ------------------------------------------------------- */

int ubi_device_init(const struct ubi_mtd *mtd, const struct ubi_crypto_config *crypto_cfg,
		    struct ubi_device **ubi)
{
	if (!mtd || !ubi) {
		LOG_ERR("NULL argument: mtd=%p ubi=%p", (const void *)mtd, (const void *)ubi);
		return -EINVAL;
	}

	if (crypto_cfg != NULL) {
		LOG_ERR("Secure backend not available");
		return -ENOTSUP;
	}

	return ubi_plain_backend()->init(mtd, ubi);
}

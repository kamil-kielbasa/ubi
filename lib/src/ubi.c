/**
 * \file    ubi.c
 * \author  Kamil Kielbasa
 * \brief   UBI public facade — runtime backend dispatch.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* Public headers: */
#include "ubi.h"

/* Internal headers: */
#include "ubi_internal.h"

/* Zephyr headers: */
#include <zephyr/logging/log.h>

/* Standard library headers: */
#include <errno.h>

/* Module defines ------------------------------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Module interface function definitions -------------------------------------------------------- */

/* Device lifecycle ----------------------------------------------------------------------------- */

int ubi_device_init(const struct ubi_flash_desc *flash, const struct ubi_secure_config *secure_cfg,
		    struct ubi_device **ubi)
{
	if (!flash || !ubi) {
		LOG_ERR("NULL argument: flash=%p ubi=%p", (const void *)flash, (const void *)ubi);
		return -EINVAL;
	}

	if (secure_cfg != NULL) {
#if defined(CONFIG_UBI_SECURE)
		return ubi_secure_backend()->init(flash, secure_cfg, ubi);
#else /* !CONFIG_UBI_SECURE */
		LOG_ERR("Secure backend not available");
		return -ENOTSUP;
#endif /* CONFIG_UBI_SECURE */
	}

	return ubi_plain_backend()->init(flash, secure_cfg, ubi);
}

int ubi_device_get_info(struct ubi_device *ubi, struct ubi_device_info *info)
{
	if (!ubi || !info) {
		LOG_ERR("NULL argument: ubi=%p info=%p", (const void *)ubi, (const void *)info);
		return -EINVAL;
	}

	return ubi->ops->get_info(ubi, info);
}

int ubi_device_erase_peb(struct ubi_device *ubi)
{
	if (!ubi) {
		LOG_ERR("ubi is NULL");
		return -EINVAL;
	}

	return ubi->ops->erase_peb(ubi);
}

int ubi_device_deinit(struct ubi_device *ubi)
{
	if (!ubi) {
		LOG_ERR("ubi is NULL");
		return -EINVAL;
	}

	return ubi->ops->deinit(ubi);
}

/* Volume management ---------------------------------------------------------------------------- */

int ubi_volume_create(struct ubi_device *ubi, const struct ubi_volume_config *vol_cfg, int *vol_id)
{
	if (!ubi || !vol_cfg || !vol_id) {
		LOG_ERR("NULL argument: ubi=%p vol_cfg=%p vol_id=%p", (const void *)ubi,
			(const void *)vol_cfg, (const void *)vol_id);
		return -EINVAL;
	}

	return ubi->ops->vol_create(ubi, vol_cfg, vol_id);
}

int ubi_volume_resize(struct ubi_device *ubi, int vol_id, const struct ubi_volume_config *vol_cfg)
{
	if (!ubi || !vol_cfg) {
		LOG_ERR("NULL argument: ubi=%p vol_cfg=%p", (const void *)ubi,
			(const void *)vol_cfg);
		return -EINVAL;
	}

	return ubi->ops->vol_resize(ubi, vol_id, vol_cfg);
}

int ubi_volume_remove(struct ubi_device *ubi, int vol_id)
{
	if (!ubi) {
		LOG_ERR("ubi is NULL");
		return -EINVAL;
	}

	return ubi->ops->vol_remove(ubi, vol_id);
}

int ubi_volume_get_info(struct ubi_device *ubi, int vol_id, struct ubi_volume_config *vol_cfg,
			size_t *alloc_lebs)
{
	if (!ubi || vol_id < 0 || !vol_cfg || !alloc_lebs) {
		LOG_ERR("Invalid argument: ubi=%p vol_id=%d vol_cfg=%p alloc_lebs=%p",
			(const void *)ubi, vol_id, (const void *)vol_cfg, (const void *)alloc_lebs);
		return -EINVAL;
	}

	return ubi->ops->vol_get_info(ubi, vol_id, vol_cfg, alloc_lebs);
}

/* LEB operations ------------------------------------------------------------------------------- */

int ubi_leb_write(struct ubi_device *ubi, int vol_id, size_t lnum, const void *buf, size_t len)
{
	if (!ubi || vol_id < 0 || !buf || len == 0) {
		LOG_ERR("Invalid argument: ubi=%p vol_id=%d buf=%p len=%zu", (const void *)ubi,
			vol_id, buf, len);
		return -EINVAL;
	}

	return ubi->ops->leb_write(ubi, vol_id, lnum, buf, len);
}

int ubi_leb_read(struct ubi_device *ubi, int vol_id, size_t lnum, size_t offset, void *buf,
		 size_t len)
{
	if (!ubi || vol_id < 0 || !buf || len == 0) {
		LOG_ERR("Invalid argument: ubi=%p vol_id=%d buf=%p len=%zu", (const void *)ubi,
			vol_id, buf, len);
		return -EINVAL;
	}

	return ubi->ops->leb_read(ubi, vol_id, lnum, offset, buf, len);
}

int ubi_leb_map(struct ubi_device *ubi, int vol_id, size_t lnum)
{
	if (!ubi || vol_id < 0) {
		LOG_ERR("Invalid argument: ubi=%p vol_id=%d", (const void *)ubi, vol_id);
		return -EINVAL;
	}

	return ubi->ops->leb_map(ubi, vol_id, lnum);
}

int ubi_leb_unmap(struct ubi_device *ubi, int vol_id, size_t lnum)
{
	if (!ubi || vol_id < 0) {
		LOG_ERR("Invalid argument: ubi=%p vol_id=%d", (const void *)ubi, vol_id);
		return -EINVAL;
	}

	return ubi->ops->leb_unmap(ubi, vol_id, lnum);
}

int ubi_leb_is_mapped(struct ubi_device *ubi, int vol_id, size_t lnum, bool *is_mapped)
{
	if (!ubi || vol_id < 0 || !is_mapped) {
		LOG_ERR("Invalid argument: ubi=%p vol_id=%d is_mapped=%p", (const void *)ubi,
			vol_id, (const void *)is_mapped);
		return -EINVAL;
	}

	return ubi->ops->leb_is_mapped(ubi, vol_id, lnum, is_mapped);
}

int ubi_leb_get_size(struct ubi_device *ubi, int vol_id, size_t lnum, size_t *size)
{
	if (!ubi || vol_id < 0 || !size) {
		LOG_ERR("Invalid argument: ubi=%p vol_id=%d size=%p", (const void *)ubi, vol_id,
			(const void *)size);
		return -EINVAL;
	}

	return ubi->ops->leb_get_size(ubi, vol_id, lnum, size);
}

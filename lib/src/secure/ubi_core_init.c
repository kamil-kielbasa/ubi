/**
 * \file    ubi_core_init.c
 * \brief   Secure backend device initialization: mode detection, format, attach.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files ------------------------------------------------------------------------------- */
#include "ubi_secure_reserved.h"
#include "ubi_secure_crypto.h"
#include "ubi_secure_ser.h"
#include "ubi_secure_types.h"
#include "ubi_internal.h"
#include "ubi_backend.h"
#include "ubi_io.h"
#include "ubi_mem.h"
#include "ubi_partition_guard.h"

#include <ubi_crypto.h>

#include <psa/crypto.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/crc.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/device.h>

#include <errno.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------ */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Forward declarations of secure backend ops -------------------------------------------------- */

static int ubi_secure_device_init(const struct ubi_mtd *mtd,
				  const struct ubi_crypto_config *crypto_cfg,
				  struct ubi_device **ubi);

/* Placeholder ops — return -ENOTSUP until PR6 implements data-path ops. */
static int secure_op_unsupported(void)
{
	LOG_ERR("Operation not yet supported by secure backend");
	return -ENOTSUP;
}

/* Cast wrappers for ops vtable (all return -ENOTSUP for now). */
static int secure_get_info(struct ubi_device *u, struct ubi_device_info *i)
{
	__ASSERT_NO_MSG(u != NULL);
	__ASSERT_NO_MSG(i != NULL);
	ARG_UNUSED(u);
	ARG_UNUSED(i);
	return secure_op_unsupported();
}

static int secure_deinit(struct ubi_device *u)
{
	__ASSERT_NO_MSG(u != NULL);

	k_mutex_lock(&u->mutex, K_FOREVER);

	/* PR5 does not populate PEB trees — only release partition + free device. */
	ubi_partition_release(u->mtd.partition_id);
	ubi_mem_device_free(u);

	return 0;
}

static int secure_erase_peb(struct ubi_device *u)
{
	__ASSERT_NO_MSG(u != NULL);
	ARG_UNUSED(u);
	return secure_op_unsupported();
}

static int secure_vol_create(struct ubi_device *u, const struct ubi_volume_config *c, int *id)
{
	__ASSERT_NO_MSG(u != NULL);
	__ASSERT_NO_MSG(c != NULL);
	__ASSERT_NO_MSG(id != NULL);
	ARG_UNUSED(u);
	ARG_UNUSED(c);
	ARG_UNUSED(id);
	return secure_op_unsupported();
}

static int secure_vol_resize(struct ubi_device *u, int id, const struct ubi_volume_config *c)
{
	__ASSERT_NO_MSG(u != NULL);
	__ASSERT_NO_MSG(c != NULL);
	ARG_UNUSED(u);
	ARG_UNUSED(id);
	ARG_UNUSED(c);
	return secure_op_unsupported();
}

static int secure_vol_remove(struct ubi_device *u, int id)
{
	__ASSERT_NO_MSG(u != NULL);
	ARG_UNUSED(u);
	ARG_UNUSED(id);
	return secure_op_unsupported();
}

static int secure_vol_get_info(struct ubi_device *u, int id, struct ubi_volume_config *c, size_t *a)
{
	__ASSERT_NO_MSG(u != NULL);
	__ASSERT_NO_MSG(c != NULL);
	__ASSERT_NO_MSG(a != NULL);
	ARG_UNUSED(u);
	ARG_UNUSED(id);
	ARG_UNUSED(c);
	ARG_UNUSED(a);
	return secure_op_unsupported();
}

static int secure_leb_write(struct ubi_device *u, int id, size_t l, const void *b, size_t n)
{
	__ASSERT_NO_MSG(u != NULL);
	__ASSERT_NO_MSG(b != NULL);
	ARG_UNUSED(u);
	ARG_UNUSED(id);
	ARG_UNUSED(l);
	ARG_UNUSED(b);
	ARG_UNUSED(n);
	return secure_op_unsupported();
}

static int secure_leb_read(struct ubi_device *u, int id, size_t l, size_t o, void *b, size_t n)
{
	__ASSERT_NO_MSG(u != NULL);
	__ASSERT_NO_MSG(b != NULL);
	ARG_UNUSED(u);
	ARG_UNUSED(id);
	ARG_UNUSED(l);
	ARG_UNUSED(o);
	ARG_UNUSED(b);
	ARG_UNUSED(n);
	return secure_op_unsupported();
}

static int secure_leb_map(struct ubi_device *u, int id, size_t l)
{
	__ASSERT_NO_MSG(u != NULL);
	ARG_UNUSED(u);
	ARG_UNUSED(id);
	ARG_UNUSED(l);
	return secure_op_unsupported();
}

static int secure_leb_unmap(struct ubi_device *u, int id, size_t l)
{
	__ASSERT_NO_MSG(u != NULL);
	ARG_UNUSED(u);
	ARG_UNUSED(id);
	ARG_UNUSED(l);
	return secure_op_unsupported();
}

static int secure_leb_is_mapped(struct ubi_device *u, int id, size_t l, bool *m)
{
	__ASSERT_NO_MSG(u != NULL);
	__ASSERT_NO_MSG(m != NULL);
	ARG_UNUSED(u);
	ARG_UNUSED(id);
	ARG_UNUSED(l);
	ARG_UNUSED(m);
	return secure_op_unsupported();
}

static int secure_leb_get_size(struct ubi_device *u, int id, size_t l, size_t *s)
{
	__ASSERT_NO_MSG(u != NULL);
	__ASSERT_NO_MSG(s != NULL);
	ARG_UNUSED(u);
	ARG_UNUSED(id);
	ARG_UNUSED(l);
	ARG_UNUSED(s);
	return secure_op_unsupported();
}

/* Backend ops vtable -------------------------------------------------------------------------- */

static const struct ubi_backend_ops secure_ops = {
	.init = ubi_secure_device_init,
	.get_info = secure_get_info,
	.deinit = secure_deinit,
	.erase_peb = secure_erase_peb,
	.vol_create = secure_vol_create,
	.vol_resize = secure_vol_resize,
	.vol_remove = secure_vol_remove,
	.vol_get_info = secure_vol_get_info,
	.leb_write = secure_leb_write,
	.leb_read = secure_leb_read,
	.leb_map = secure_leb_map,
	.leb_unmap = secure_leb_unmap,
	.leb_is_mapped = secure_leb_is_mapped,
	.leb_get_size = secure_leb_get_size,
};

const struct ubi_backend_ops *ubi_secure_backend(void)
{
	return &secure_ops;
}

/* Static function definitions ----------------------------------------------------------------- */

/**
 * \brief Validate crypto config: all callbacks must be non-NULL.
 */
static int validate_crypto_cfg(const struct ubi_crypto_config *cfg)
{
	__ASSERT_NO_MSG(cfg != NULL);
	if (!cfg->get_key_id || !cfg->check_freshness || !cfg->sync_freshness || !cfg->event_cb) {
		LOG_ERR("Crypto config has NULL callbacks");
		return -EINVAL;
	}

	if (cfg->policy.allowed_key_versions_len == 0 || !cfg->policy.allowed_key_versions) {
		LOG_ERR("Crypto config has empty allowlist");
		return -EINVAL;
	}

	return 0;
}

/**
 * \brief Check if the requested write key version is in the allowlist.
 */
static bool key_version_is_allowed(const struct ubi_crypto_policy *policy, uint8_t kv)
{
	__ASSERT_NO_MSG(policy != NULL);
	__ASSERT_NO_MSG(policy->allowed_key_versions != NULL);
	for (size_t i = 0; i < policy->allowed_key_versions_len; i++) {
		if (policy->allowed_key_versions[i] == kv) {
			return true;
		}
	}
	return false;
}

/**
 * \brief Detect mode from reserved PEBs: blank, secure, or plain.
 *
 * \retval 0     All PEBs classified.
 * \retval -EIO  Flash error.
 */
static int detect_reserved_mode(const struct ubi_mtd *mtd, bool *any_blank, bool *any_secure,
				bool *any_plain)
{
	__ASSERT_NO_MSG(mtd != NULL);
	__ASSERT_NO_MSG(any_blank != NULL);
	__ASSERT_NO_MSG(any_secure != NULL);
	__ASSERT_NO_MSG(any_plain != NULL);

	*any_blank = false;
	*any_secure = false;
	*any_plain = false;

	for (size_t peb = 0; peb < UBI_DEV_HDR_NR_OF_RES_PEBS; peb++) {
		bool is_secure = false;
		bool is_blank = false;

		const int ret = ubi_secure_res_peb_detect_mode(mtd, peb, &is_secure, &is_blank);

		if (ret != 0) {
			LOG_ERR("detect_reserved_mode: PEB %zu detection failed: %d", peb, ret);
			return ret;
		}

		if (is_blank) {
			*any_blank = true;
		} else if (is_secure) {
			*any_secure = true;
		} else {
			/* Has content but not secure magic → plain. */
			*any_plain = true;
		}
	}

	return 0;
}

/**
 * \brief Format a blank device in secure mode.
 */
static int secure_format(const struct ubi_mtd *mtd, const struct ubi_crypto_config *crypto_cfg,
			 struct ubi_device *ubi_dev)
{
	__ASSERT_NO_MSG(mtd != NULL);
	__ASSERT_NO_MSG(crypto_cfg != NULL);
	__ASSERT_NO_MSG(ubi_dev != NULL);

	const struct flash_area *fa = NULL;
	int ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		return -EIO;
	}

	/* Build initial device header. */
	struct ubi_dev_hdr dev_hdr = {
		.magic = UBI_DEV_HDR_MAGIC,
		.version = UBI_DEV_HDR_VERSION,
		.offset = fa->fa_off,
		.size = fa->fa_size,
		.revision = 0,
		.vol_count = 0,
		.vol_id_watermark = 0,
	};

	dev_hdr.hdr_crc =
		crc32_ieee((const uint8_t *)&dev_hdr, sizeof(dev_hdr) - sizeof(dev_hdr.hdr_crc));

	flash_area_close(fa);

	struct ubi_dev_secure_meta dev_meta = {
		.write_active_key_version = crypto_cfg->policy.requested_write_key_version,
		.vid_next_counter_floor = 0,
	};
	memset(dev_meta.reserved0, 0, sizeof(dev_meta.reserved0));

	/* Commit encrypted reserved PEBs. */
	ret = ubi_secure_res_peb_commit(mtd, crypto_cfg, &dev_hdr, &dev_meta, NULL, 0,
					crypto_cfg->policy.requested_write_key_version, 0);
	if (ret != 0 && ret != -EROFS) {
		LOG_ERR("Secure format commit failure");
		return ret;
	}

	/* Populate ubi_device fields from formatted state. */
	ubi_dev->vol_id_watermark = 0;
	ubi_dev->vol_count = 0;
	ubi_dev->read_only_degraded = (ret == -EROFS);

	return 0;
}

/**
 * \brief Attach to an existing secure device.
 */
static int secure_attach(const struct ubi_mtd *mtd, const struct ubi_crypto_config *crypto_cfg,
			 struct ubi_device *ubi_dev)
{
	__ASSERT_NO_MSG(mtd != NULL);
	__ASSERT_NO_MSG(crypto_cfg != NULL);
	__ASSERT_NO_MSG(ubi_dev != NULL);

	/* Scan and authenticate reserved PEBs. */
	struct ubi_secure_res_peb_scan scan = { 0 };
	int ret = ubi_secure_res_peb_scan(mtd, crypto_cfg, &scan);

	if (ret != 0) {
		LOG_ERR("Secure reserved PEB scan failure");
		return ret;
	}

	if (scan.auth_count == 0) {
		LOG_ERR("No authenticated reserved PEBs found");
		return -EIO;
	}

	/* Validate allowlist: the device's write_active_key_version must be allowlisted. */
	if (!key_version_is_allowed(&crypto_cfg->policy, scan.dev_meta.write_active_key_version)) {
		LOG_ERR("On-flash write_active_key_version %u not in allowlist",
			scan.dev_meta.write_active_key_version);
		return -EACCES;
	}

	/* Authenticate volume headers. */
	struct ubi_vol_hdr vol_hdrs[CONFIG_UBI_MAX_NR_OF_VOLUMES] = { 0 };

	if (scan.dev_hdr.vol_count > 0) {
		ret = ubi_secure_res_peb_read_vol_hdrs(mtd, crypto_cfg, &scan, vol_hdrs,
						       CONFIG_UBI_MAX_NR_OF_VOLUMES);
		if (ret != 0) {
			LOG_ERR("Volume header authentication failure");
			return ret;
		}
	}

	/* Build freshness descriptor and call check_freshness. */
	const struct ubi_crypto_freshness freshness = {
		.device_revision = (uint64_t)scan.dev_hdr.revision,
		.global_sqnum = 0, /* Will be populated after data PEB scan in PR6. */
	};

	const enum ubi_crypto_rollback_verdict verdict =
		crypto_cfg->check_freshness(&freshness, crypto_cfg->user_data);

	if (verdict == UBI_CRYPTO_ROLLBACK_REJECT) {
		LOG_ERR("Freshness check rejected — rollback detected");
		return -EACCES;
	}

	/* Populate device state from authenticated headers. */
	ubi_dev->vol_id_watermark = scan.dev_hdr.vol_id_watermark;
	ubi_dev->vol_count = scan.dev_hdr.vol_count;
	ubi_dev->read_only_degraded = (scan.auth_count < UBI_SECURE_RES_PEB_NR_ACTIVE);

	return 0;
}

/* Public function ----------------------------------------------------------------------------- */

static int ubi_secure_device_init(const struct ubi_mtd *mtd,
				  const struct ubi_crypto_config *crypto_cfg,
				  struct ubi_device **ubi)
{
	__ASSERT_NO_MSG(mtd != NULL);
	__ASSERT_NO_MSG(crypto_cfg != NULL);
	__ASSERT_NO_MSG(ubi != NULL);

	int ret = validate_crypto_cfg(crypto_cfg);

	if (ret != 0) {
		LOG_ERR("Crypto config validation failed");
		*ubi = NULL;
		return ret;
	}

	/* Initialize PSA crypto subsystem. */
	const psa_status_t psa_ret = psa_crypto_init();

	if (psa_ret != PSA_SUCCESS) {
		LOG_ERR("PSA crypto init failure: %d", (int)psa_ret);
		*ubi = NULL;
		return -EIO;
	}

	/* Validate that the write key version is allowlisted. */
	if (!key_version_is_allowed(&crypto_cfg->policy,
				    crypto_cfg->policy.requested_write_key_version)) {
		LOG_ERR("Requested write key version %u not in allowlist",
			crypto_cfg->policy.requested_write_key_version);
		*ubi = NULL;
		return -EINVAL;
	}

	/* Acquire partition. */
	ret = ubi_partition_acquire(mtd->partition_id);
	if (ret != 0) {
		LOG_ERR("Partition %u already in use", mtd->partition_id);
		*ubi = NULL;
		return -EBUSY;
	}

	/* Allocate device structure. */
	struct ubi_device *ubi_dev = NULL;

	ret = ubi_mem_device_alloc(&ubi_dev);
	if (ret != 0) {
		LOG_ERR("Device allocation failure");
		ubi_partition_release(mtd->partition_id);
		*ubi = NULL;
		return ret;
	}

	k_mutex_init(&ubi_dev->mutex);
	ubi_dev->mtd = *mtd;
	ubi_dev->mode = UBI_MODE_SECURE;
	ubi_dev->ops = ubi_secure_backend();
	ubi_dev->free_pebs.lessthan_fn = ubi_cache_cmp;
	ubi_dev->dirty_pebs.lessthan_fn = ubi_cache_cmp;
	sys_slist_init(&ubi_dev->bad_pebs);
	ubi_dev->vols.lessthan_fn = ubi_cache_cmp;

	/* Validate flash geometry. */
	const struct flash_area *fa = NULL;

	ret = flash_area_open(ubi_dev->mtd.partition_id, &fa);
	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		goto exit;
	}

	if (!device_is_ready(flash_area_get_device(fa))) {
		LOG_ERR("Flash area is not ready");
		flash_area_close(fa);
		ret = -ENODEV;
		goto exit;
	}

	flash_area_close(fa);

	/* Detect mode: blank, secure, or plain. */
	bool any_blank = false;
	bool any_secure = false;
	bool any_plain = false;

	ret = detect_reserved_mode(mtd, &any_blank, &any_secure, &any_plain);
	if (ret != 0) {
		LOG_ERR("Reserved PEB mode detection failed");
		goto exit;
	}

	/* Mode mismatch: plain media + secure config. */
	if (any_plain) {
		LOG_ERR("Plain media detected but secure config provided — mode mismatch");
		ret = -EPROTO;
		goto exit;
	}

	if (any_secure) {
		/* Existing secure media → attach. */
		ret = secure_attach(mtd, crypto_cfg, ubi_dev);
	} else {
		/* All blank → format. */
		ret = secure_format(mtd, crypto_cfg, ubi_dev);
	}

	if (ret != 0) {
		LOG_ERR("Secure %s failed: %d", any_secure ? "attach" : "format", ret);
		goto exit;
	}

	*ubi = ubi_dev;
	return 0;

exit:
	ubi_mem_device_free(ubi_dev);
	ubi_partition_release(mtd->partition_id);
	*ubi = NULL;
	return ret;
}

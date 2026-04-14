/**
 * \file    ubi_backend.h
 * \brief   UBI backend operations interface for runtime backend selection.
 *
 * \copyright Copyright (c) 2026
 */

/* Include guard ------------------------------------------------------------------------------- */
#ifndef UBI_BACKEND_H
#define UBI_BACKEND_H

/* Forward declarations ------------------------------------------------------------------------ */

struct ubi_mtd;
struct ubi_device;
struct ubi_volume_config;

/* Types and type definitions ------------------------------------------------------------------ */

/**
 * \brief Backend mode for a UBI device.
 *
 * Selected at runtime during ubi_device_init() based on the caller-supplied
 * crypto_cfg pointer.
 */
enum ubi_device_mode {
	UBI_MODE_PLAIN = 0,
	UBI_MODE_SECURE = 1,
};

/**
 * \brief Backend operations vtable.
 *
 * Each backend (plain, secure) provides an implementation of this interface.
 * The facade layer (ubi.c) dispatches all public API calls through these ops.
 *
 * All ops except init receive the device mutex already unlocked; the backend
 * implementation is responsible for locking/unlocking as needed.
 */
struct ubi_backend_ops {
	/* Device lifecycle */
	int (*init)(const struct ubi_mtd *mtd, struct ubi_device **ubi);
	int (*get_info)(struct ubi_device *ubi, struct ubi_device_info *info);
	int (*deinit)(struct ubi_device *ubi);
	int (*erase_peb)(struct ubi_device *ubi);

	/* Volume management */
	int (*vol_create)(struct ubi_device *ubi, const struct ubi_volume_config *vol_cfg,
			  int *vol_id);
	int (*vol_resize)(struct ubi_device *ubi, int vol_id,
			  const struct ubi_volume_config *vol_cfg);
	int (*vol_remove)(struct ubi_device *ubi, int vol_id);
	int (*vol_get_info)(struct ubi_device *ubi, int vol_id, struct ubi_volume_config *vol_cfg,
			    size_t *alloc_lebs);

	/* LEB operations */
	int (*leb_write)(struct ubi_device *ubi, int vol_id, size_t lnum, const void *buf,
			 size_t len);
	int (*leb_read)(struct ubi_device *ubi, int vol_id, size_t lnum, size_t offset, void *buf,
			size_t len);
	int (*leb_map)(struct ubi_device *ubi, int vol_id, size_t lnum);
	int (*leb_unmap)(struct ubi_device *ubi, int vol_id, size_t lnum);
	int (*leb_is_mapped)(struct ubi_device *ubi, int vol_id, size_t lnum, bool *is_mapped);
	int (*leb_get_size)(struct ubi_device *ubi, int vol_id, size_t lnum, size_t *size);
};

/**
 * \brief Get the plain backend operations.
 */
const struct ubi_backend_ops *ubi_plain_backend(void);

#endif /* UBI_BACKEND_H */

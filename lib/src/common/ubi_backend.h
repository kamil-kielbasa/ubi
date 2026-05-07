/**
 * \file    ubi_backend.h
 * \brief   UBI backend operations interface for runtime backend selection.
 * \author  Kamil Kielbasa
 *
 * \copyright Copyright (c) 2026
 */

/* Include guard -------------------------------------------------------------------------------- */

#ifndef UBI_BACKEND_H
#define UBI_BACKEND_H

/* Forward declarations ------------------------------------------------------------------------- */

struct ubi_flash_desc;
struct ubi_device;
struct ubi_crypto_config;
struct ubi_volume_config;

/* Types and type definitions ------------------------------------------------------------------- */

/**
 * \brief Backend mode for a UBI device.
 *
 * Selected at runtime during ubi_device_init() based on the caller-supplied
 * crypto_cfg pointer.
 */
enum ubi_device_mode {
	UBI_MODE_PLAIN = 0, /**< Plain backend — CRC-only integrity. */
	UBI_MODE_SECURE = 1, /**< Secure backend — AES-128-CCM authenticated encryption. */
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
	int (*init)(const struct ubi_flash_desc *flash, const struct ubi_crypto_config *crypto_cfg,
		    struct ubi_device **ubi); /**< Initialize device. */
	int (*deinit)(struct ubi_device *ubi); /**< Shut down and free resources. */
	int (*get_info)(struct ubi_device *ubi,
			struct ubi_device_info *info); /**< Query device state. */
	int (*erase_peb)(struct ubi_device *ubi); /**< Reclaim one dirty PEB. */

	/* Volume management */
	int (*vol_create)(struct ubi_device *ubi, const struct ubi_volume_config *vol_cfg,
			  int *vol_id); /**< Create a new volume. */
	int (*vol_resize)(struct ubi_device *ubi, int vol_id,
			  const struct ubi_volume_config *vol_cfg); /**< Resize a volume. */
	int (*vol_remove)(struct ubi_device *ubi, int vol_id); /**< Remove a volume. */
	int (*vol_get_info)(struct ubi_device *ubi, int vol_id, struct ubi_volume_config *vol_cfg,
			    size_t *alloc_lebs); /**< Query volume configuration. */

	/* LEB operations */
	int (*leb_write)(struct ubi_device *ubi, int vol_id, size_t lnum, const void *buf,
			 size_t len); /**< Write data to a LEB. */
	int (*leb_read)(struct ubi_device *ubi, int vol_id, size_t lnum, size_t offset, void *buf,
			size_t len); /**< Read data from a LEB. */
	int (*leb_map)(struct ubi_device *ubi, int vol_id, size_t lnum); /**< Map a LEB. */
	int (*leb_unmap)(struct ubi_device *ubi, int vol_id, size_t lnum); /**< Unmap a LEB. */
	int (*leb_is_mapped)(struct ubi_device *ubi, int vol_id, size_t lnum,
			     bool *is_mapped); /**< Check LEB mapping. */
	int (*leb_get_size)(struct ubi_device *ubi, int vol_id, size_t lnum,
			    size_t *size); /**< Get LEB data size. */
};

/**
 * \brief Get the plain backend operations.
 */
const struct ubi_backend_ops *ubi_plain_backend(void);

#if defined(CONFIG_UBI_CRYPTO)

/**
 * \brief Get the secure backend operations.
 */
const struct ubi_backend_ops *ubi_secure_backend(void);

#endif /* CONFIG_UBI_CRYPTO */

#endif /* UBI_BACKEND_H */

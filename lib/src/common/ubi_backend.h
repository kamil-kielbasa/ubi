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
 * The facade layer (ubi.c) selects and calls through these ops.
 */
struct ubi_backend_ops {
	/**
	 * \brief Initialize (attach or format) a UBI device.
	 *
	 * On success the implementation must set ubi_device.mode and
	 * ubi_device.ops before returning.
	 */
	int (*init)(const struct ubi_mtd *mtd, struct ubi_device **ubi);
};

/**
 * \brief Get the plain backend operations.
 */
const struct ubi_backend_ops *ubi_plain_backend(void);

#endif /* UBI_BACKEND_H */

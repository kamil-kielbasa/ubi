/**
 * \file    ubi_secure_ops.h
 * \author  Kamil Kielbasa
 * \brief   Secure backend operation declarations for the ops vtable.
 *
 * \copyright Copyright (c) 2026
 */

/* Include guard -------------------------------------------------------------------------------- */

#ifndef UBI_SECURE_OPS_H
#define UBI_SECURE_OPS_H

/* Include files -------------------------------------------------------------------------------- */

/* Public headers: */
#include "ubi.h"

/* Standard library headers: */
#include <stdbool.h>
#include <stddef.h>

/* Secure backend operation declarations -------------------------------------------------------- */

/* Forward declarations for opaque internal types referenced by the ops below. */
struct ubi_volume;
struct ubi_flash_desc;
struct ubi_secure_config;

/* ubi_core_init.c */
int ubi_secure_device_init(const struct ubi_flash_desc *flash,
			   const struct ubi_secure_config *secure_cfg, struct ubi_device **ubi);

/* ubi_secure_runtime.c */
int ubi_secure_device_get_info(struct ubi_device *ubi, struct ubi_device_info *info);
int ubi_secure_device_deinit(struct ubi_device *ubi);
int ubi_secure_device_erase_peb(struct ubi_device *ubi);

/* ubi_secure_volume.c */
int ubi_secure_volume_create(struct ubi_device *ubi, const struct ubi_volume_config *vol_cfg,
			     int *vol_id);
int ubi_secure_volume_resize(struct ubi_device *ubi, int vol_id,
			     const struct ubi_volume_config *vol_cfg);
int ubi_secure_volume_remove(struct ubi_device *ubi, int vol_id);
int ubi_secure_volume_get_info(struct ubi_device *ubi, int vol_id,
			       struct ubi_volume_config *vol_cfg, size_t *alloc_lebs);

/* ubi_secure_leb.c */
int ubi_secure_leb_write(struct ubi_device *ubi, int vol_id, size_t lnum, const void *buf,
			 size_t len);
int ubi_secure_leb_read(struct ubi_device *ubi, int vol_id, size_t lnum, size_t offset, void *buf,
			size_t len);
int ubi_secure_leb_map(struct ubi_device *ubi, int vol_id, size_t lnum);
int ubi_secure_leb_unmap(struct ubi_device *ubi, int vol_id, size_t lnum);
int ubi_secure_leb_is_mapped(struct ubi_device *ubi, int vol_id, size_t lnum, bool *is_mapped);
int ubi_secure_leb_get_size(struct ubi_device *ubi, int vol_id, size_t lnum, size_t *size);

#endif /* UBI_SECURE_OPS_H */

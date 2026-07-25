/**
 * \file    ubi_plain_ops.h
 * \brief   Plain backend operation declarations for the ops vtable.
 * \author  Kamil Kielbasa
 *
 * \copyright Copyright (c) 2026
 */

/* Include guard -------------------------------------------------------------------------------- */

#ifndef UBI_PLAIN_OPS_H
#define UBI_PLAIN_OPS_H

/* Include files -------------------------------------------------------------------------------- */

/* Public headers: */
#include "ubi.h"

/* Standard library headers: */
#include <stdbool.h>
#include <stddef.h>

/* Plain backend operation declarations --------------------------------------------------------- */

/* ubi_plain_core_runtime.c */
int ubi_plain_device_get_info(struct ubi_device *ubi, struct ubi_device_info *info);
int ubi_plain_device_deinit(struct ubi_device *ubi);
int ubi_plain_device_erase_peb(struct ubi_device *ubi);

/* ubi_plain_volume.c */
int ubi_plain_volume_create(struct ubi_device *ubi, const struct ubi_volume_config *vol_cfg,
			    int *vol_id);
int ubi_plain_volume_resize(struct ubi_device *ubi, int vol_id,
			    const struct ubi_volume_config *vol_cfg);
int ubi_plain_volume_remove(struct ubi_device *ubi, int vol_id);
int ubi_plain_volume_get_info(struct ubi_device *ubi, int vol_id, struct ubi_volume_config *vol_cfg,
			      size_t *alloc_lebs);

/* ubi_plain_leb.c */
int ubi_plain_leb_write(struct ubi_device *ubi, int vol_id, size_t lnum, const void *buf,
			size_t len);
int ubi_plain_leb_write_at(struct ubi_device *ubi, int vol_id, size_t lnum, size_t offset,
			   const void *buf, size_t len);
int ubi_plain_leb_read(struct ubi_device *ubi, int vol_id, size_t lnum, size_t offset, void *buf,
		       size_t len);
int ubi_plain_leb_map(struct ubi_device *ubi, int vol_id, size_t lnum);
int ubi_plain_leb_unmap(struct ubi_device *ubi, int vol_id, size_t lnum);
int ubi_plain_leb_is_mapped(struct ubi_device *ubi, int vol_id, size_t lnum, bool *is_mapped);
int ubi_plain_leb_get_size(struct ubi_device *ubi, int vol_id, size_t lnum, size_t *size);

#endif /* UBI_PLAIN_OPS_H */

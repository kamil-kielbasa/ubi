/**
 * \file    ubi_secure_backend.c
 * \author  Kamil Kielbasa
 * \brief   Secure backend operations vtable singleton.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files ------------------------------------------------------------------------------- */
#include "ubi_secure_ops.h"
#include "ubi_backend.h"

/* Backend ops vtable -------------------------------------------------------------------------- */

const struct ubi_backend_ops *ubi_secure_backend(void)
{
	static const struct ubi_backend_ops ops = {
		.init = ubi_secure_device_init,
		.get_info = ubi_secure_device_get_info,
		.deinit = ubi_secure_device_deinit,
		.erase_peb = ubi_secure_device_erase_peb,
		.vol_create = ubi_secure_volume_create,
		.vol_resize = ubi_secure_volume_resize,
		.vol_remove = ubi_secure_volume_remove,
		.vol_get_info = ubi_secure_volume_get_info,
		.leb_write = ubi_secure_leb_write,
		.leb_read = ubi_secure_leb_read,
		.leb_map = ubi_secure_leb_map,
		.leb_unmap = ubi_secure_leb_unmap,
		.leb_is_mapped = ubi_secure_leb_is_mapped,
		.leb_get_size = ubi_secure_leb_get_size,
	};

	return &ops;
}

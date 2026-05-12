/**
 * \file    ubi_api_contract.h
 *
 * \author  Kamil Kielbasa
 *
 * \brief   Shared parity-test bodies for the UBI public API.
 *
 * \details Each helper encodes one assertion contract that is identical
 *          for the plain and the secure backend.  The plain
 *          (tests_ubi_error_handling) and the secure
 *          (tests_ubi_secure_error_handling) suites both call into these
 *          helpers so the assertion lives in exactly one place; if the
 *          contract changes, both backends update together.  Each helper
 *          assumes the caller passes an already-initialised device and
 *          handles its own deinit -- the helper does not touch
 *          init/deinit.
 *
 * \copyright Copyright (c) 2026
 */

/* Include guard -------------------------------------------------------------------------------- */

#ifndef UBI_API_CONTRACT_H
#define UBI_API_CONTRACT_H

/* Include files -------------------------------------------------------------------------------- */

#include <ubi.h>

#include <zephyr/ztest.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Module interface function definitions -------------------------------------------------------- */

static inline void ubi_contract_get_info_null_device(struct ubi_device *ubi)
{
	struct ubi_device_info info = { 0 };
	zassert_equal(-EINVAL, ubi_device_get_info(NULL, &info));
}

static inline void ubi_contract_get_info_null_info(struct ubi_device *ubi)
{
	zassert_equal(-EINVAL, ubi_device_get_info(ubi, NULL));
}

static inline void ubi_contract_leb_get_size_no_volumes(struct ubi_device *ubi)
{
	size_t size = 0;
	zassert_equal(-ENOENT, ubi_leb_get_size(ubi, 0, 0, &size));
}

static inline void ubi_contract_leb_get_size_null(struct ubi_device *ubi)
{
	zassert_equal(-EINVAL, ubi_leb_get_size(ubi, 0, 0, NULL));
}

static inline void ubi_contract_leb_get_size_out_of_range(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "soor",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	size_t size = 0;
	zassert_equal(-EACCES, ubi_leb_get_size(ubi, vol_id, 5, &size));
}

static inline void ubi_contract_leb_get_size_unmapped(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "gsum",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	size_t size = 0;
	zassert_equal(-ENOENT, ubi_leb_get_size(ubi, vol_id, 0, &size));
}

static inline void ubi_contract_leb_get_size_vol_not_found(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "s_vnf",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	size_t size = 0;
	zassert_equal(-ENOENT, ubi_leb_get_size(ubi, 999, 0, &size));
}

static inline void ubi_contract_leb_is_mapped_null(struct ubi_device *ubi)
{
	zassert_equal(-EINVAL, ubi_leb_is_mapped(ubi, 0, 0, NULL));
}

static inline void ubi_contract_leb_map_already_mapped_is_noop(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "noop_m",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_ok(ubi_leb_map(ubi, vol_id, 0));

	struct ubi_device_info info_before = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_before));

	zassert_ok(ubi_leb_map(ubi, vol_id, 0));

	struct ubi_device_info info_after = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_after));

	zassert_equal(info_before.free_peb_count, info_after.free_peb_count,
		      "No-op map should not consume a PEB");
}

static inline void ubi_contract_leb_map_then_write(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "maptw",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Map LEB 0 (reserves a PEB without data) */
	zassert_ok(ubi_leb_map(ubi, vol_id, 0));

	bool mapped = false;
	zassert_ok(ubi_leb_is_mapped(ubi, vol_id, 0, &mapped));
	zassert_true(mapped);

	/* Write data to already-mapped LEB 0 (should overwrite) */
	const uint8_t data[] = { 0x01, 0x02, 0x03, 0x04 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint8_t rdata[4] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, data, sizeof(data));
}

static inline void ubi_contract_leb_map_vol_not_found(struct ubi_device *ubi)
{
	int ret = ubi_leb_map(ubi, 999, 0);
	zassert_equal(ret, -ENOENT);
}

static inline void ubi_contract_leb_read_beyond_data_size(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "over",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xAA, 0xBB, 0xCC, 0xDD };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint8_t rb[8] = { 0 };
	int ret = ubi_leb_read(ubi, vol_id, 0, 0, rb, sizeof(rb));
	zassert_equal(-EINVAL, ret, "Read beyond data_size should return -EINVAL");
}

static inline void ubi_contract_leb_read_last_byte_at_boundary(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "bdry",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	/* Read last byte */
	uint8_t rb = 0;
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 7, &rb, 1));
	zassert_equal(0x80, rb);
}

static inline void ubi_contract_leb_read_null_buffer(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "rdnull",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_equal(-EINVAL, ubi_leb_read(ubi, vol_id, 0, 0, NULL, 10));
}

static inline void ubi_contract_leb_read_with_offset(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "offrd",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint8_t rdata[4] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 4, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, &data[4], sizeof(rdata));
}

static inline void ubi_contract_leb_unmap_no_volumes(struct ubi_device *ubi)
{
	zassert_equal(-ENOENT, ubi_leb_unmap(ubi, 0, 0));
}

static inline void ubi_contract_leb_unmap_out_of_range(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "umoor",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* lnum > leb_count should return -EACCES */
	zassert_equal(-EACCES, ubi_leb_unmap(ubi, vol_id, 5));
}

static inline void ubi_contract_leb_unmap_unmapped(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "umtest",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));
}

static inline void ubi_contract_leb_unmap_unmapped_is_idempotent(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "idem_u",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));
}

static inline void ubi_contract_leb_unmap_vol_not_found(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "u_vnf",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_equal(-ENOENT, ubi_leb_unmap(ubi, 999, 0));
}

static inline void ubi_contract_leb_write_no_volumes(struct ubi_device *ubi)
{
	const uint8_t data[] = { 0xAA };
	/* No volumes => -ENOENT */
	zassert_equal(-ENOENT, ubi_leb_write(ubi, 0, 0, data, sizeof(data)));
}

static inline void ubi_contract_leb_write_null_buffer(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "wrtest",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_equal(-EINVAL, ubi_leb_write(ubi, vol_id, 0, NULL, 10));
}

static inline void ubi_contract_leb_write_out_of_range_lnum(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "oor_w",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xAA };
	/* lnum > leb_count should return -EACCES */
	zassert_equal(-EACCES, ubi_leb_write(ubi, vol_id, 3, data, sizeof(data)));
}

static inline void ubi_contract_leb_write_vol_not_found(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "w_vnf",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xAA };
	zassert_equal(-ENOENT, ubi_leb_write(ubi, 999, 0, data, sizeof(data)));
}

static inline void ubi_contract_leb_write_zero_length(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "zerolen",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	uint8_t data = 0x42;
	zassert_equal(-EINVAL, ubi_leb_write(ubi, vol_id, 0, &data, 0));
}

static inline void ubi_contract_static_volume_write_allowed(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "stwr",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xCA, 0xFE };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint8_t rdata[2] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, data, sizeof(data));
}

static inline void ubi_contract_volume_create_idempotent(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "idem",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id_1 = -1;
	int vol_id_2 = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id_1));
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id_2));
	zassert_equal(vol_id_1, vol_id_2);

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.volume_count);
}

static inline void ubi_contract_volume_create_invalid_type(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "badtp",
		.type = 42,
		.leb_count = 1,
	};
	int vol_id = -1;
	zassert_equal(-EINVAL, ubi_volume_create(ubi, &cfg, &vol_id));
}

static inline void ubi_contract_volume_create_name_max_valid(struct ubi_device *ubi)
{
	struct ubi_volume_config cfg = {
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	memset(cfg.name, 0, sizeof(cfg.name));
	memset(cfg.name, 'B', UBI_VOLUME_NAME_MAX_LEN - 1);

	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));
}

static inline void ubi_contract_volume_create_no_space(struct ubi_device *ubi)
{
	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));

	const struct ubi_volume_config cfg = {
		.name = "huge",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = info.total_peb_count + 1,
	};
	int vol_id = -1;

	zassert_equal(-ENOSPC, ubi_volume_create(ubi, &cfg, &vol_id));
}

static inline void ubi_contract_volume_create_null_params(struct ubi_device *ubi)
{
	int vol_id = -1;
	const struct ubi_volume_config cfg = {
		.name = "test",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	zassert_equal(-EINVAL, ubi_volume_create(NULL, &cfg, &vol_id));
	zassert_equal(-EINVAL, ubi_volume_create(ubi, NULL, &vol_id));
	zassert_equal(-EINVAL, ubi_volume_create(ubi, &cfg, NULL));
}

static inline void ubi_contract_volume_create_zero_lebs(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "zero",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 0,
	};
	int vol_id = -1;
	zassert_equal(-EINVAL, ubi_volume_create(ubi, &cfg, &vol_id));
}

static inline void ubi_contract_volume_get_info_detailed(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "detail",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 3,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Write to 2 LEBs */
	const uint8_t data[] = { 0x11 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 2, data, sizeof(data)));

	struct ubi_volume_config out_cfg = { 0 };
	size_t alloc = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &out_cfg, &alloc));

	zassert_equal(3, out_cfg.leb_count);
	zassert_equal(UBI_VOLUME_TYPE_DYNAMIC, out_cfg.type);
	zassert_equal(2, alloc, "Should have 2 allocated LEBs");
	zassert_true(strncmp(out_cfg.name, "detail", 6) == 0);
}

static inline void ubi_contract_volume_get_info_no_volumes(struct ubi_device *ubi)
{
	struct ubi_volume_config cfg = { 0 };
	size_t alloc = 0;
	zassert_equal(-ENOENT, ubi_volume_get_info(ubi, 0, &cfg, &alloc));
}

static inline void ubi_contract_volume_get_info_nonexistent(struct ubi_device *ubi)
{
	struct ubi_volume_config cfg = { 0 };
	size_t alloc = 0;
	zassert_equal(-ENOENT, ubi_volume_get_info(ubi, 999, &cfg, &alloc));
}

static inline void ubi_contract_volume_remove_no_volumes(struct ubi_device *ubi)
{
	zassert_equal(-ENOENT, ubi_volume_remove(ubi, 0));
}

static inline void ubi_contract_volume_remove_nonexistent(struct ubi_device *ubi)
{
	zassert_equal(-ENOENT, ubi_volume_remove(ubi, 999));
}

static inline void ubi_contract_volume_resize_expand_enospc(struct ubi_device *ubi)
{
	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));

	const struct ubi_volume_config cfg = {
		.name = "rspc",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Try to resize to more LEBs than the partition can hold */
	const struct ubi_volume_config big_cfg = {
		.name = "rspc",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = info.total_peb_count + 10,
	};
	zassert_equal(-ENOSPC, ubi_volume_resize(ubi, vol_id, &big_cfg));
}

static inline void ubi_contract_volume_resize_grow_preserves_data(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "grow",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xAA, 0xBB };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	const struct ubi_volume_config cfg4 = {
		.name = "grow",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 4,
	};
	zassert_ok(ubi_volume_resize(ubi, vol_id, &cfg4));

	/* Verify old data */
	uint8_t rb[2] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rb, sizeof(rb)));
	zassert_mem_equal(rb, data, sizeof(data));

	/* New LEBs should be accessible */
	const uint8_t d3[] = { 0xCC };
	zassert_ok(ubi_leb_write(ubi, vol_id, 3, d3, sizeof(d3)));

	uint8_t r3[1] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 3, 0, r3, sizeof(r3)));
	zassert_equal(0xCC, r3[0]);
}

static inline void ubi_contract_volume_resize_no_volumes(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "nope",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};

	/* No volumes exist, resize should return -ENOENT */
	zassert_equal(-ENOENT, ubi_volume_resize(ubi, 0, &cfg));
}

static inline void ubi_contract_volume_resize_nonexistent(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "none",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	zassert_equal(-ENOENT, ubi_volume_resize(ubi, 999, &cfg));
}

static inline void ubi_contract_volume_resize_null_config(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "rsnul",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* NULL vol_cfg should return -EINVAL */
	zassert_equal(-EINVAL, ubi_volume_resize(ubi, vol_id, NULL));
}

static inline void ubi_contract_volume_resize_same_size(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "dyn",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_equal(-ECANCELED, ubi_volume_resize(ubi, vol_id, &cfg));
}

static inline void ubi_contract_volume_resize_static(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "static",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	struct ubi_volume_config new_cfg = cfg;
	new_cfg.leb_count = 4;
	zassert_equal(-ECANCELED, ubi_volume_resize(ubi, vol_id, &new_cfg));
}

static inline void ubi_contract_volume_resize_zero_lebs_rejected(struct ubi_device *ubi)
{
	const struct ubi_volume_config cfg = {
		.name = "rzero",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const struct ubi_volume_config zero_cfg = {
		.name = "rzero",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 0,
	};
	zassert_equal(-EINVAL, ubi_volume_resize(ubi, vol_id, &zero_cfg));
}

#endif /* UBI_API_CONTRACT_H */

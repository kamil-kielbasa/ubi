---
orphan: true
---

# Developer Notes

```{note}
**Implementation map.** This page is for UBI maintainers and contributors.
It is intentionally **not in the sidebar** and is not part of the v1.0.0
public documentation surface. Public users should read
{doc}`/architecture/plain_architecture` and the {doc}`/reference/api` instead.
```

## Plain UBI source files

| File | Role |
|------|------|
| `lib/include/ubi.h` | Public API — all structures and function declarations |
| `lib/src/ubi_core_init.c` | Device initialization — format, scan, mount |
| `lib/src/ubi_core_runtime.c` | Device runtime — get_info, erase_peb, deinit, test API |
| `lib/src/ubi_volume.c` | Volume management — create, resize, remove, get_info |
| `lib/src/ubi_leb.c` | LEB operations — read, write (copy-on-write), map, unmap (idempotent), is_mapped, get_size |
| `lib/src/ubi_cache.c` | Red-black tree comparator and search helpers |
| `lib/src/ubi_internal.h` | Shared internal types (`ubi_device`, `ubi_volume`) and helpers |
| `lib/src/ubi_cache.h` | RBT and linked-list item types |
| `lib/src/ubi_io.h` | On-flash header structures and constants |
| `lib/src/ubi_io_metadata.c` | Metadata I/O — device and volume header read/write |
| `lib/src/ubi_io_data.c` | Data I/O — EC/VID header and LEB data read/write, flash write/erase fault injection |
| `lib/src/ubi_flash_res_peb.h` | Reserved PEB state types and API declarations |
| `lib/src/ubi_flash_res_peb.c` | Reserved PEB scanning, recovery, overwrite, and commit |
| `lib/src/ubi_partition_guard.h` | Single-handle-per-partition registry API |
| `lib/src/ubi_partition_guard.c` | Static bitfield registry preventing double-init of the same partition |
| `lib/src/ubi_mem.h` | Memory abstraction layer API — device, volume, leaf, scratch allocators |
| `lib/src/ubi_mem.c` | Static (k_mem_slab) and heap (k_malloc) backend implementations |

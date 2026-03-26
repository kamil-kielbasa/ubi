# Architecture Guide

This document provides a comprehensive overview of the UBI subsystem internals. For an introduction to what UBI is and why it exists, see the [Introduction](introduction.md).

## Flash Storage Primer

Raw flash memory (NAND or NOR) differs from block devices like SD cards or eMMC in several important ways:

- **Erase before write** — a flash cell must be erased before it can be written. Erasing sets all bits to `0xFF`.
- **Erase granularity** — erasure operates on large blocks (erase blocks), typically 4 KB to 256 KB.
- **Write granularity** — writes operate on smaller units (write blocks), typically 1 to 16 bytes.
- **Limited endurance** — each erase block supports a finite number of erase cycles (typically 10,000 to 100,000) before it becomes unreliable.
- **Bad blocks** — blocks can fail at any point during the device lifetime.

Without wear-leveling, repeatedly writing to the same logical location would exhaust a small set of physical blocks while the rest remain unused. UBI solves this by dynamically remapping logical blocks to physical blocks, always choosing the least-worn block for new writes.

**Key terminology:**

| Term | Meaning |
|------|---------|
| PEB  | Physical Erase Block — a hardware erase unit on the flash chip |
| LEB  | Logical Erase Block — a virtual block exposed to the application |
| EC   | Erase Counter — tracks how many times a PEB has been erased |
| VID  | Volume Identifier — metadata linking a PEB to a volume and LEB |
| EBA  | Erase Block Association — the mapping table from LEBs to PEBs |

---

## Architecture Overview

```
+-----------------------------------------------------+
|                   Application                        |
+-----------------------------------------------------+
            |                          ^
            | ubi_leb_write()          | ubi_leb_read()
            | ubi_volume_create()      | ubi_volume_get_info()
            | ubi_device_init()        | ubi_device_get_info()
            v                          |
+-----------------------------------------------------+
|                     UBI Layer                        |
|                                                      |
|  +---------------+  +---------------+  +---------+  |
|  | Volume Mgmt   |  | LEB I/O       |  | Wear-   |  |
|  | create/remove |  | read/write    |  | Level   |  |
|  | resize/info   |  | map/unmap     |  | Engine  |  |
|  +---------------+  +---------------+  +---------+  |
|                                                      |
|  +----------------------------------------------+   |
|  |           PEB Management (RBT Cache)          |   |
|  |  free_pebs | dirty_pebs | bad_pebs | vols     |   |
|  +----------------------------------------------+   |
+-----------------------------------------------------+
            |                          ^
            | flash_area_write()       | flash_area_read()
            | flash_area_erase()       |
            v                          |
+-----------------------------------------------------+
|          Zephyr Flash Area API (Flash Map)           |
+-----------------------------------------------------+
            |                          ^
            v                          |
+-----------------------------------------------------+
|              Flash Hardware (NOR / NAND)             |
+-----------------------------------------------------+
```

**Source files:**

| File | Role |
|------|------|
| `lib/include/ubi.h` | Public API — all structures and function declarations |
| `lib/src/ubi_core.c` | Device lifecycle — init, deinit, get_info, erase_peb |
| `lib/src/ubi_volume.c` | Volume management — create, resize, remove, get_info |
| `lib/src/ubi_leb.c` | LEB operations — read, write, map, unmap, is_mapped, get_size |
| `lib/src/ubi_cache.c` | Red-black tree comparator and search helpers |
| `lib/src/ubi_internal.h` | Shared internal types (`ubi_device`, `ubi_volume`) and helpers |
| `lib/src/ubi_cache.h` | RBT and linked-list item types |
| `lib/src/ubi_io.h` | On-flash header structures and constants |
| `lib/src/ubi_io.c` | Low-level flash I/O, header read/write, dual-bank logic |

---

## On-Flash Layout

UBI reserves the first two PEBs (PEB 0 and PEB 1) for device and volume metadata, stored in a dual-bank configuration for crash resilience. The remaining PEBs (2 through N-1) are data blocks available for volume use.

```
Flash Partition
+====================+====================+=====+====================+
| PEB 0 (Reserved)   | PEB 1 (Reserved)   | ... | PEB N-1            |
| Device Header Bank | Device Header Bank |     | Data Block         |
+====================+====================+=====+====================+

Reserved PEB Layout (PEB 0 and PEB 1 are mirrors):

Offset 0x000  +----------------------+
              | Device Header (32 B) |  magic, version, revision, vol_count, CRC
              +----------------------+
Offset 0x020  | Volume 0 Hdr  (48 B) |  magic, vol_id, name, type, leb_count, CRC
              +----------------------+
Offset 0x050  | Volume 1 Hdr  (48 B) |
              +----------------------+
              |         ...          |  (up to CONFIG_UBI_MAX_NR_OF_VOLUMES)
              +----------------------+


Data PEB Layout (PEB 2 through PEB N-1):

Offset 0x000  +----------------------+
              | EC Header    (16 B)  |  magic, version, erase_counter, CRC
              +----------------------+
Offset 0x010  | VID Header   (32 B)  |  magic, vol_id, leb_num, sqnum, data_size, CRC
              +----------------------+
Offset 0x030  |                      |
              |     User Data        |  up to (erase_block_size - 48) bytes
              |                      |
              +----------------------+
```

When a data PEB is **free** (not assigned to any volume), its VID header area is erased (`0xFF`). The EC header is always present on valid PEBs.

---

## Header Structures

All headers are aligned to 16 bytes and protected by CRC-32/IEEE (`crc32_ieee()` from Zephyr's `<zephyr/sys/crc.h>`). The CRC covers all fields except the `hdr_crc` field itself.

### Erase Counter (EC) Header — 16 bytes

Present on every data PEB. Tracks how many times this block has been erased.

```
Offset  Size  Field
------  ----  -----
0x00    4     magic       (0x55424923)
0x04    1     version     (1)
0x05    3     padding
0x08    4     ec          erase counter value
0x0C    4     hdr_crc     CRC-32 of bytes 0x00..0x0B
```

### Volume Identifier (VID) Header — 32 bytes

Present on data PEBs that are mapped to a volume. Links a PEB to a specific volume and LEB.

```
Offset  Size  Field
------  ----  -----
0x00    4     magic       (0x55424921)
0x04    1     version     (1)
0x05    3     padding
0x08    4     lnum        logical erase block number within the volume
0x0C    4     vol_id      volume identifier
0x10    8     sqnum       global sequence number (monotonically increasing)
0x18    4     data_size   size of user data in bytes
0x1C    4     hdr_crc     CRC-32 of bytes 0x00..0x1B
```

The `sqnum` field is critical for crash recovery. During the PEB scan at init, if two PEBs claim the same (vol_id, lnum) pair, the one with the higher `sqnum` wins.

### Device Header — 32 bytes

Stored on reserved PEB 0 and PEB 1. Describes the overall UBI device.

```
Offset  Size  Field
------  ----  -----
0x00    4     magic       (0x55424925)
0x04    1     version     (1)
0x05    3     padding
0x08    4     offset      offset of the first volume header
0x0C    4     size        device size
0x10    4     revision    header revision counter (incremented on each metadata update)
0x14    4     vol_count   number of volumes
0x18    4     padding
0x1C    4     hdr_crc     CRC-32 of bytes 0x00..0x1B
```

### Volume Header — 48 bytes

One per volume, stored sequentially after the device header on the reserved PEBs.

```
Offset  Size  Field
------  ----  -----
0x00    4     magic       (0x55424926)
0x04    1     version     (1)
0x05    1     vol_type    0 = static, 1 = dynamic
0x06    2     padding
0x08    4     vol_id      unique volume identifier
0x0C    4     leb_count   number of LEBs allocated to this volume
0x10    12    padding
0x1C    16    name        null-terminated volume name (max 16 bytes including '\0')
0x2C    4     hdr_crc     CRC-32 of bytes 0x00..0x2B
```

---

## In-RAM Data Structures

When `ubi_device_init()` runs, it scans the flash and builds an in-RAM cache of PEB states. This cache is the heart of UBI — all runtime decisions (which PEB to write to, which blocks are dirty, etc.) are made from these structures without re-reading flash.

### Overview

```
struct ubi_device (112 B)
|
|-- mutex                       Zephyr mutex for thread safety
|-- mtd                         Flash partition config (partition_id, block sizes)
|
|-- free_pebs (Red-Black Tree, keyed by erase counter)
|   |
|   |   Holds PEBs that are erased and available for new writes.
|   |   The minimum node (lowest EC) is selected for writes (wear-leveling).
|   |
|   |       ec:3         Nodes are struct ubi_rbt_item {
|   |      /    \            .key   = erase_counter,
|   |   ec:1   ec:7         .value.pnum = PEB index
|   |          /    \    }
|   |       ec:5  ec:12
|   |
|   `-- Each node points to a physical PEB on flash:
|           ec:1 --> PEB 5  [EC hdr: ec=1 | VID: 0xFF (empty) | ...]
|           ec:3 --> PEB 8  [EC hdr: ec=3 | VID: 0xFF (empty) | ...]
|           ec:5 --> PEB 14 [EC hdr: ec=5 | VID: 0xFF (empty) | ...]
|
|-- dirty_pebs (Red-Black Tree, keyed by erase counter)
|   |
|   |   Holds PEBs that contain stale data and need erasure before reuse.
|   |   Populated when a LEB is overwritten or unmapped.
|   |
|   |       ec:4
|   |      /    \
|   |   ec:2   ec:9
|   |
|   `-- Each node points to a PEB with outdated data:
|           ec:2 --> PEB 3  [EC hdr: ec=2 | VID: old data | ...]
|           ec:4 --> PEB 11 [EC hdr: ec=4 | VID: old data | ...]
|
|-- bad_pebs (Singly-Linked List)
|   |
|   |   Holds PEBs with I/O errors (invalid EC headers, failed erases/writes).
|   |   Entries are struct ubi_list_item { .pnum, .erase_count }
|   |
|   `-- [PEB 22, ec:~7] --> [PEB 45, ec:~3] --> NULL
|
|       NOTE: Bad block list is NOT persisted to flash.
|             It is lost on reboot and rebuilt during the next init scan.
|
|-- vols (Red-Black Tree, keyed by volume ID)
|   |
|   |   Maps volume IDs to struct ubi_volume pointers.
|   |
|   |     vol_id:0             Nodes are struct ubi_rbt_item {
|   |      /     \                 .key   = volume_id,
|   |  vol_id:1  vol_id:5         .value.vol = &ubi_volume
|   |                          }
|   |
|   `-- Each ubi_volume (48 B) contains:
|
|       struct ubi_volume
|       |-- vol_idx         Index in the reserved PEB header table
|       |-- vol_id          Unique volume identifier
|       |-- cfg             { name[16], type (static|dynamic), leb_count }
|       |-- eba_tbl_count   Number of mapped LEBs
|       `-- eba_tbl (Red-Black Tree, keyed by LEB number)
|           |
|           |   Per-volume mapping from logical to physical blocks.
|           |
|           |     leb:2             Nodes are struct ubi_rbt_item {
|           |    /     \                .key   = LEB_number,
|           | leb:0   leb:5            .value.pnum = PEB_index
|           |                      }
|           |
|           `-- Each node points to the PEB holding that LEB's data:
|                   leb:0 --> PEB 7  [EC hdr | VID: vol=0,leb=0,sq=42 | payload]
|                   leb:2 --> PEB 19 [EC hdr | VID: vol=0,leb=2,sq=50 | payload]
|                   leb:5 --> PEB 31 [EC hdr | VID: vol=0,leb=5,sq=55 | payload]
|
`-- global_sqnum            Monotonically increasing sequence number for writes
`-- vol_next_id             Next volume ID to assign
```

### How the Structures Relate to Flash

Every PEB on flash is tracked by exactly one of these structures at any time:

```
                          +------------------+
                          |   Physical Flash |
                          +------------------+
                          | PEB 0  (reserved)|----> Device + Volume headers (Bank 1)
                          | PEB 1  (reserved)|----> Device + Volume headers (Bank 2)
                          |------------------|
  free_pebs RBT --------->| PEB 2  (free)    |  EC hdr present, VID = 0xFF
  free_pebs RBT --------->| PEB 3  (free)    |  EC hdr present, VID = 0xFF
                          |------------------|
  vol[0].eba_tbl -------->| PEB 4  (vol0/L0) |  EC hdr + VID(vol=0,leb=0) + data
  vol[0].eba_tbl -------->| PEB 5  (vol0/L1) |  EC hdr + VID(vol=0,leb=1) + data
                          |------------------|
  vol[1].eba_tbl -------->| PEB 6  (vol1/L0) |  EC hdr + VID(vol=1,leb=0) + data
                          |------------------|
  dirty_pebs RBT -------->| PEB 7  (dirty)   |  EC hdr + VID (stale data)
                          |------------------|
  bad_pebs list --------->| PEB 8  (bad)     |  Unreadable or failed I/O
                          +------------------+

  Rule: PEB 0,1 are always reserved.
        Every other PEB is in exactly ONE of:
        - free_pebs      (erased, ready for use)
        - Some volume's eba_tbl  (in use, holds live data)
        - dirty_pebs     (contains stale data, awaiting erasure)
        - bad_pebs       (defective, excluded from use)
```

### Memory Usage

| Structure | Size per entry | Allocated |
|-----------|---------------|-----------|
| `ubi_device` | 112 B | Once per device |
| `ubi_rbt_item` | 16 B | Once per PEB + once per volume |
| `ubi_volume` | 48 B | Once per volume |
| `ubi_list_item` | 12 B | Once per bad PEB |

All allocations are dynamic (`k_malloc`). Static RAM usage is zero.

---

## PEB Lifecycle

A Physical Erase Block moves through the following states during normal operation:

```
                          +-------+
           ubi_device_    |       |   ubi_device_init()
           erase_peb() -->| FREE  |<-- (fresh flash: all PEBs start here)
           (ec += 1)      |       |    
                          +---+---+
                              |
                              | leb_write() or leb_map()
                              | (rb_get_min selects lowest EC)
                              v
                        +-----------+
                        |           |
                        | ALLOCATED |   In a volume's eba_tbl
                        | (in use)  |   VID header links to vol_id + leb_num
                        |           |
                        +-----+-----+
                              |
                              | leb_write() (overwrite) or leb_unmap()
                              | Old PEB moved to dirty_pebs
                              v
                          +-------+
                          |       |
                          | DIRTY |   Stale data, awaiting erasure
                          |       |
                          +---+---+
                              |
                              | ubi_device_erase_peb()
                              | (erase flash, increment EC, write new EC hdr)
                              v
                          +-------+
                          | FREE  |   Back in free_pebs, ready for reuse
                          +-------+

  At ANY point, if a flash I/O operation fails:

                          +-------+
              I/O error   |       |
           ------------>  |  BAD  |   Moved to bad_pebs linked list
                          |       |   Excluded from all future operations
                          +-------+
```

---

## Device Initialization

`ubi_device_init()` is the most complex function in UBI. It handles two fundamentally different scenarios: initializing a brand-new (never-used) flash device, and re-mounting an existing device after a reboot.

### Flow Overview

```
ubi_device_init(mtd, &ubi)
        |
        v
  Allocate ubi_device, init mutex, init RBTs
        |
        v
  Check: is device mounted?
  (read PEB 0 and PEB 1, look for valid device headers)
        |
        +--- NO (fresh flash) -------> Phase 0: First-Time Mount
        |                                  |
        +--- YES (reboot) --+              |
        |                   |              v
        |                   |     Write device header to PEB 0 and PEB 1
        |                   |     Erase PEBs 2..N-1
        |                   |     Write EC headers (ec=0) to each
        |                   |              |
        v                   v              |
  +--------------------------------------------+
  | Phase 1: Read Device Header                |
  |   Read device header from reserved PEBs    |
  |   For each volume in vol_count:            |
  |     Read volume header                     |
  |     Allocate ubi_volume + ubi_rbt_item     |
  |     Insert into vols RBT                   |
  +--------------------------------------------+
                    |
                    v
  +--------------------------------------------+
  | Phase 2: Compute Average Erase Count       |
  |   Scan PEBs 2..N-1                         |
  |   Read EC headers, sum valid erase counts  |
  |   ec_avg = ec_sum / ec_count               |
  |   (Used as fallback EC for bad blocks)     |
  +--------------------------------------------+
                    |
                    v
  +--------------------------------------------+
  | Phase 3: PEB Scan & Classification         |
  |   For each PEB from 2 to N-1:             |
  |                                            |
  |   3.1  EC header invalid?                  |
  |         --> bad_pebs (ec = ec_avg)         |
  |                                            |
  |   3.2  EC valid, VID = 0xFF (empty)?       |
  |         --> free_pebs (key = ec)           |
  |                                            |
  |   3.3  EC valid, VID invalid CRC?          |
  |         --> bad_pebs (ec from EC hdr)      |
  |                                            |
  |   3.4  EC valid, VID valid:                |
  |     3.4.1  Track max sqnum for global_seqnr|
  |     3.4.2  Volume not found in vols RBT?   |
  |             --> dirty_pebs (orphaned)      |
  |     3.4.3  LEB >= vol.leb_count?           |
  |             --> dirty_pebs (out of range)  |
  |     3.4.4  LEB not in vol.eba_tbl?         |
  |             --> insert into vol.eba_tbl    |
  |     3.4.5  LEB already in vol.eba_tbl?     |
  |             Compare sqnum:                 |
  |             - new < existing: new-->dirty  |
  |             - new > existing: old-->dirty, |
  |               new replaces in eba_tbl      |
  +--------------------------------------------+
                    |
                    v
            Return ubi_device*
```

### First-Time Mount vs. Reboot

| Aspect | First-Time Mount | Reboot (Re-mount) |
|--------|------------------|--------------------|
| Device header on PEB 0/1 | Not present | Already written |
| Phase 0 | Erase all data PEBs, write EC headers with `ec=0` | Skipped entirely |
| Phase 1–3 | Runs (all PEBs will be free) | Runs (reconstructs volumes from existing data) |
| Volume data | None — empty EBA tables | Reconstructed from VID headers on flash |
| Dirty PEBs | None | May exist from incomplete writes before reboot |
| Bad PEBs | Detected from Phase 3 scan | Detected fresh (previous list was in RAM only) |

### Sequence Number Conflict Resolution

When two PEBs claim the same `(vol_id, leb_num)` pair (e.g., a write was interrupted and both the old and new PEB survive), UBI resolves the conflict using the `sqnum` field in the VID header:

- The PEB with the **higher** `sqnum` is the newer write and is kept in the EBA table.
- The PEB with the **lower** `sqnum` is moved to `dirty_pebs` for later erasure.

This ensures that even after an unexpected power loss, the most recent successful write survives.

---

## Thread Safety

Since v0.5.0, all public API functions acquire a per-device Zephyr mutex (`struct k_mutex`) before accessing any shared state. This means:

- Multiple threads can safely call UBI functions on the same device concurrently.
- The mutex provides mutual exclusion (one thread at a time), not read-write differentiation.
- The mutex is initialized in `ubi_device_init()` and held for the duration of each API call.
- Callers do not need to provide their own locking.

See the [Roadmap](roadmap.md) for the planned upgrade to a fair read-write lock.

---

## Wear-Leveling

UBI implements a **greedy minimum-erase-count** wear-leveling strategy.

### Write Path

When writing to a LEB, UBI always selects the free PEB with the **lowest** erase counter:

```c
struct rbnode *min = rb_get_min(&ubi->free_pebs);
```

Since `free_pebs` is a red-black tree keyed by erase count, `rb_get_min()` returns the least-worn block in O(log n) time.

### Erase Path

When erasing dirty PEBs, UBI also processes the one with the **lowest** erase counter first:

```c
struct rbnode *min = rb_get_min(&ubi->dirty_pebs);
```

After erasing, the PEB's erase counter is incremented and it is moved back to `free_pebs`.

### Effect

This two-sided greedy approach naturally distributes wear across all PEBs:

- Least-worn blocks are consumed first for writes, giving them more cycles.
- Least-worn dirty blocks are recycled first, keeping the counter distribution tight.
- Over time, all PEBs converge toward a similar erase count.

---

## Dual-Bank Mechanism

UBI stores device and volume metadata on two reserved PEBs (PEB 0 and PEB 1) as mirrors. This dual-bank approach protects against metadata corruption from unexpected power loss during header updates.

### Write Sequence

When metadata changes (volume created, removed, or resized), UBI writes to both banks sequentially:

```
1. Erase PEB 0
2. Write updated headers to PEB 0    --> BANK1_VALID state
3. Erase PEB 1
4. Write updated headers to PEB 1    --> BANKS_VALID state
```

If power fails between steps 2 and 4, PEB 0 contains the new data while PEB 1 still has the old data. On next boot, this is detectable.

### Bank States

```
+----------------+    Both banks readable,    +---------------+
| BANKS_INVALID  |    same CRC & revision     | BANKS_VALID   |
| (unformatted)  | ---- First mount ---------> | (normal ops)  |
+----------------+                            +-------+-------+
                                                      |
                                           Power loss during update
                                                      |
                               +--------------+-------+--------------+
                               |                                     |
                               v                                     v
                        +--------------+                     +--------------+
                        | BANK1_VALID  |                     | BANK2_VALID  |
                        | (degraded)   |                     | (degraded)   |
                        +--------------+                     +--------------+
```

**Current limitation:** Recovery from a single-bank-valid state (restoring the damaged bank from the valid one) is not yet implemented. In this state, `ubi_device_init()` returns `-ENOSYS`.

---

## Volume Management

### Volume Types

| Type | Enum | Description |
|------|------|-------------|
| Static | `UBI_VOLUME_TYPE_STATIC` (0) | Fixed content. Cannot be resized after creation. |
| Dynamic | `UBI_VOLUME_TYPE_DYNAMIC` (1) | Content can change. Supports runtime resizing. |

### Create

`ubi_volume_create()` assigns a unique volume ID, writes a new volume header to both reserved PEBs (incrementing the device revision), and adds the volume to the in-RAM `vols` RBT. The PEBs for the volume are **not** pre-allocated — they are claimed from `free_pebs` on-demand when LEBs are written or mapped.

If a volume with the same name already exists, the function returns successfully with the existing volume's ID (idempotent behavior).

### Resize

`ubi_volume_resize()` is only supported for dynamic volumes. It updates the `leb_count` in the volume header on both reserved PEBs and adjusts the in-RAM configuration. If the volume is shrunk, LEBs beyond the new limit are unmapped and their PEBs are moved to `dirty_pebs`.

### Remove

`ubi_volume_remove()` unmaps all LEBs (moving their PEBs to `dirty_pebs`), removes the volume header from the reserved PEBs, and frees the in-RAM structures.


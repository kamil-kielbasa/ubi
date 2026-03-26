# Roadmap

Planned features and improvements for UBI on Zephyr. Items are listed by priority.

## Feature Overview

| Feature | Priority | Status | Description |
|---------|----------|--------|-------------|
| Dual-bank recovery | High | Planned | Restore corrupted bank from the valid one on boot |
| Permanent bad block tracking | High | Planned | Persist bad block records to flash across reboots |
| Write retry mechanism | Medium | Planned | Retry flash writes on transient failures |
| Bad block torture test | Medium | Planned | Stress-test suspect PEBs before retiring them |
| Volume module simplification | Medium | Planned | Deduplicate ubi_volume.c with shared helpers |
| Read-write locking | Medium | Planned | Allow concurrent readers with exclusive writer access |
| User-space tools | Low | Planned | Port Linux UBI CLI utilities to Zephyr shell |

## Details

### Dual-Bank Recovery

UBI writes device and volume headers to two reserved PEBs (dual-bank). If power is lost during a metadata update, one bank may be left with stale or corrupt data. Currently, this state (`BANK1_VALID` or `BANK2_VALID`) returns `-ENOSYS`.

The goal is to detect the degraded state during `ubi_device_init()` and automatically restore the damaged bank from the valid one before proceeding with normal initialization.

### Permanent Bad Block Tracking

Bad blocks are currently tracked in RAM only. If a PEB fails at runtime, it is excluded until the next reboot, at which point the information is lost and the block may be used again (and fail again).

This feature would stress-test suspicious blocks with multiple erase attempts. If a block consistently fails, it is marked as permanently bad and stored in a reserved area of flash so the record persists across reboots.

### Write Retry Mechanism

Flash write operations can fail due to transient conditions (voltage fluctuations, marginal cells). This feature would introduce a configurable retry count for write operations before declaring a PEB bad.

### Bad Block Torture Test

When a PEB fails an erase or write operation, perform multiple erase+write cycles to determine whether the failure is transient or permanent. If the block passes the torture test, return it to the free pool; otherwise, retire it as a permanent bad block (requires the permanent bad block tracking feature).

This corresponds to the `/** TODO: Torture bad blocks. */` placeholder in `ubi_core.c`.

### Volume Module Simplification

Deduplicate repeated patterns in `ubi_volume.c` by extracting shared helpers:

1. **`ubi_dev_hdr_update_and_recalc()`** — read device header, bump revision, recalc CRC (used by create, resize, remove).
2. **`ubi_move_peb_to_dirty_pool()`** — read EC header, insert into dirty tree (used by resize, remove).
3. **`ubi_reclaim_volume_pebs()`** — iterate EBA table and reclaim PEBs to dirty pool (used by resize, remove).
4. **`ubi_volume_remove()` in-memory vol_idx update** — replace the O(n) NVM-read loop with an O(m) in-memory traversal.

Estimated reduction: ~150-200 lines (~30-40% of file).

### Read-Write Locking

The current mutex provides mutual exclusion but does not differentiate between readers and writers. A fair read-write lock would allow multiple concurrent readers while ensuring writers eventually gain access without starvation.

### User-Space Tools

Port the essential Linux UBI user-space utilities (`ubinfo`, `ubimkvol`, `ubirmvol`, `ubiattach`) to Zephyr shell commands, giving developers familiar tools for interactive device management during development and debugging.

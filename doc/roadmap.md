# Roadmap

Planned features and improvements for UBI on Zephyr. Items are listed by priority.

## Feature Overview

| Feature | Priority | Status | Description |
|---------|----------|--------|-------------|
| Dual-bank recovery | High | Planned | Restore corrupted bank from the valid one on boot |
| Permanent bad block tracking | High | Planned | Persist bad block records to flash across reboots |
| Write retry mechanism | Medium | Planned | Retry flash writes on transient failures |
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

### Read-Write Locking

The current mutex provides mutual exclusion but does not differentiate between readers and writers. A fair read-write lock would allow multiple concurrent readers while ensuring writers eventually gain access without starvation.

### User-Space Tools

Port the essential Linux UBI user-space utilities (`ubinfo`, `ubimkvol`, `ubirmvol`, `ubiattach`) to Zephyr shell commands, giving developers familiar tools for interactive device management during development and debugging.

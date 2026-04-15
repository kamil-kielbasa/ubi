# Secure Volume Lifecycle

**What this page covers:** End-to-end lifecycle of secure volumes, including
hidden anchor behavior during create, resize, shrink, remove, and reboot.

**Prerequisites:** [Secure Architecture](secure_architecture.md) §7.9, §9.8, §11.4–§11.7.

## Overview

Every secure volume has one hidden anchor PEB in addition to its user-visible
LEBs.  The anchor carries an authenticated zero-length secure LEB record that
preserves the per-volume LEB floor across reclaim, unmap, and shrink.

```
volume create  ─► anchor init (INTERNAL_ANCHOR_LNUM)
                   ├── leb_write_counter = 0
                   └── vid_counter assigned from global next_vid_counter
```

## Create

`ubi_volume_create()` performs:

1. Commit reserved metadata (device header + volume headers) — writes the new
   volume's `leb_count` to the dual-bank reserved area.
2. Create the hidden anchor PEB — an authenticated zero-length secure LEB at
   `INTERNAL_ANCHOR_LNUM`, consuming one free PEB.
3. On anchor failure: roll back the RAM state (no volume visible to the caller).

After create, `reserved_peb_count = leb_count + 1` (user LEBs + anchor).

## Resize (grow)

`ubi_volume_resize()` with a larger `leb_count`:

1. Commit updated reserved metadata with the new count.
2. The anchor PEB is not rewritten — its floor already covers the existing range.
3. New LEBs are unmapped until explicitly written.

After grow, `reserved_peb_count = new_leb_count + 1`.

## Shrink

`ubi_volume_resize()` with a smaller `leb_count`:

1. Commit updated reserved metadata with the new count.
2. Tail LEBs whose `lnum >= new_leb_count` are moved from the EBA table to the
   dirty list in RAM.
3. The anchor PEB is not rewritten — it inherits the newest floor, and the
   smaller `leb_count` prevents tail LEBs from being mapped on reboot.

**Key distinction (§11.7):** Shrink is committed in reserved metadata _before_
the dirty PEBs are physically erased.  After reboot without erase, tail PEBs
whose authenticated `lnum` is out of range are recovered as _dirty_, not
_mapped_.

After shrink, `reserved_peb_count = new_leb_count + 1`.

## Remove

`ubi_volume_remove()` performs:

1. Commit reserved metadata with `vol_count - 1`.
2. Move all user LEBs + the anchor PEB to the dirty list.
3. Save `vid_next_counter_floor` in the secure device header (§9.8.5) to
   prevent counter reset after removing all volumes.

After remove of the last volume: `volume_count = 0`, `reserved_peb_count = 0`.
The VID counter floor is preserved — creating a new volume after reboot will
use counter values above the previous lifetime.

## Unmap

`ubi_leb_unmap()` is an in-memory transition from `mapped` to `dirty`.  No
on-flash tombstone is written.  Until the dirty PEB is erased, the old
authenticated record survives — reboot before erase may reconstruct the
old mapping.

The hidden anchor is _not_ affected by unmap of user LEBs.

## Erase and Reclaim

`ubi_device_erase_peb()` picks the dirtiest PEB and erases it.  During erase,
the anchor witness check (§11.6) may detect that the erased PEB carried the
last known floor:

1. If `dirty_entry.vid_counter > anchor.vid_counter`, the anchor is rewritten
   with the newer floor _before_ the dirty PEB is erased.
2. The old anchor PEB becomes dirty and is eventually recycled — this is how
   the anchor participates in wear-leveling.

The emergency reserve (§11.5) ensures at least one free PEB is available for
anchor migration during write-path operations.

## Reboot Recovery

On `ubi_device_init()`:

1. Reserved metadata (dual-bank) is read + authenticated → provides volume
   list, `vid_next_counter_floor`.
2. Data PEBs are scanned — each authenticated VID is classified:
   - Anchor PEB (INTERNAL_ANCHOR_LNUM): duplicate resolution by `vid_sqnum`.
   - User PEB: mapped if `lnum < leb_count`, otherwise dirty (orphan/shrunk).
3. `next_vid_counter` is reconstructed as `max(floor, max_seen_counter + 1)`.
4. Stale anchor duplicates (from migration) are resolved: only the one with
   the highest `vid_sqnum` survives; the other becomes dirty.

# Secure Recovery Notes

**What this page covers:** Recovery behavior specific to secure mode after
crashes, reboots, and corner-case erase sequences.

**Prerequisites:** [Secure Architecture](secure_architecture.md) §9.8, §10,
[Volume Lifecycle](secure_volume_lifecycle.md).

## Recovery Principles

Secure mode inherits the plain core's recovery semantics with two additions:

1. **Authentication gates classification.** Every PEB header must pass AEAD
   verification before the PEB is trusted.  Unauthenticated PEBs are classified
   as dirty, not mapped.
2. **Hidden anchor provides floor continuity.** The per-volume anchor PEB
   ensures the LEB floor is never lost, even when all user LEBs are unmapped
   or erased.

## Scenario: `unmap → reboot` (before erase)

The unmapped PEB's authenticated VID survives on flash.  On reinit:

- The PEB is rediscovered and the old mapping is reconstructed.
- The anchor is unaffected — its floor is at least as fresh as the user PEB.
- From the user's perspective, the unmap did not persist.

## Scenario: `unmap → erase → reboot`

After erase, the user PEB is physically gone.  If it carried the newest floor:

- The erase path's witness check already rewrote the anchor with the floor.
- On reinit, the anchor supplies the floor — no counter regression.

## Scenario: `shrink → reboot` (before erase)

Shrink commits the new `leb_count` to reserved metadata immediately.  On reinit:

- Tail PEBs whose `lnum >= new_leb_count` are classified as dirty (orphan),
  even though their authenticated VID is intact.
- The volume's `leb_count` reflects the shrunken size.
- No data loss for LEBs within the new range.

## Scenario: `shrink → erase → reboot`

Same as above, but dirty tail PEBs are erased before reboot:

- If a tail PEB carried the newest floor, the anchor witness check fires
  during erase and rewrites the anchor first.
- On reinit: clean state, all PEBs accounted for.

## Scenario: `remove all volumes → reboot → create`

When the last volume is removed:

- `vid_next_counter_floor` is saved in the secure device header (§9.8.5).
- All user PEBs and anchors are moved to dirty.
- On reinit: `next_vid_counter` is restored from the floor.
- A new volume's writes start from a counter above the previous lifetime.

## Scenario: anchor migration during erase

When a dirty PEB has a higher `vid_counter` than the current anchor:

1. The anchor is rewritten to a new free PEB with the dirty PEB's floor.
2. The old anchor PEB is moved to dirty.
3. The original dirty PEB is now safe to erase.

This means the anchor PEB physically migrates across PEBs, participating in
normal wear-leveling.  No PEB is permanently trapped as an anchor.

## Scenario: stale anchor after reboot

After anchor migration, two PEBs carry anchor VIDs (old + new).  On reinit:

- Both are discovered during the data-PEB scan.
- Duplicate resolution selects the one with the higher `vid_sqnum`.
- The stale copy is moved to dirty.

## Emergency Reserve

The write path calls `ubi_secure_try_refill_reserve()` before checking
`free_peb_count`.  If the free pool is empty but dirty PEBs exist, one
dirty PEB is erased (with anchor witness check) to restore headroom.

This prevents deadlock where a write needs a free PEB for both the data
write and a potential anchor migration.

## Dual-Bank Reserved Metadata

Reserved metadata (device header, volume headers, device meta) is stored in
a dual-bank layout.  On reboot:

- Both banks are read and authenticated.
- The bank with the higher revision wins.
- If one bank fails authentication, the device enters degraded mode
  (`read_only_degraded = true`) — user data reads continue, but reserved
  metadata mutations are blocked until the corrupted bank is recovered.

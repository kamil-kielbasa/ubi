# Roadmap

Planned features and improvements for UBI on Zephyr. Items are listed by priority.

## Feature Overview

| Feature | Priority | Status | Description |
|---------|----------|--------|-------------|
| Crypto layer (authenticated encryption) | High | Design | AES-128-CCM encryption of all on-flash structures via PSA Crypto |
| Volume module simplification | Medium | Planned | Deduplicate ubi_volume.c with shared helpers |
| Read-write locking | Medium | Planned | Allow concurrent readers with exclusive writer access |
| User-space tools | Low | Planned | Port Linux UBI CLI utilities to Zephyr shell |

## Details

### Crypto Layer (Authenticated Encryption)

Optional security layer that encrypts and authenticates all UBI on-flash structures (device headers, volume headers, EC headers, VID headers, LEB data) using AES-128-CCM via the PSA Crypto API. Controlled by `CONFIG_UBI_CRYPTO`.

Key capabilities:
- **ESSIV nonces** bind EC/VID/data blocks to their physical flash location (prevents PEB relocation attacks).
- **Anti-rollback** persists the global sequence number to secure storage via user callbacks.
- **Key rotation** re-encrypts all PEBs in-place with a new key (crash-safe, PEB-by-PEB).
- **External AAD callback** lets applications bind LEB data to application-specific context.

Full design: [design_proposal_crypto.md](design_proposal_crypto.md).

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

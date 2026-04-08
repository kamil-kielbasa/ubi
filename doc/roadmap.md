# Roadmap

Planned features and improvements for UBI on Zephyr. Items are listed by priority.

## Feature Overview

| Feature | Priority | Status | Description |
|---------|----------|--------|-------------|
| Crypto layer (authenticated encryption) | High | Design | AES-128-CCM encryption of all on-flash structures via PSA Crypto |
| Recovery correctness for data PEB commit order | High | Planned | Change data-PEB write order to EC → DATA → VID and fix init classification of free versus uncommitted PEBs |
| Shell commands | Low | Planned | Port Linux UBI CLI utilities to Zephyr shell commands |

## Details

### Crypto Layer (Authenticated Encryption)

Optional security layer that encrypts and authenticates all UBI on-flash structures (device headers, volume headers, EC headers, VID headers, LEB data) using AES-128-CCM via the PSA Crypto API. Controlled by `CONFIG_UBI_CRYPTO`.

Key capabilities:
- **ESSIV nonces** bind EC/VID/data blocks to their physical flash location (prevents PEB relocation attacks).
- **Anti-rollback** persists the global sequence number to secure storage via user callbacks.
- **Key rotation** re-encrypts all PEBs in-place with a new key (crash-safe, PEB-by-PEB).
- **External AAD callback** lets applications bind LEB data to application-specific context.

Full design: [design_proposal_crypto.md](design_proposal_crypto.md).

### Shell Commands

Port the essential Linux UBI user-space utilities (`ubinfo`, `ubimkvol`, `ubirmvol`, `ubiattach`) to Zephyr shell commands, giving developers familiar tools for interactive device management during development and debugging.

## Recovery correctness for data PEB commit order

### Make VID the commit-visible mapping record

- Change the data-PEB write sequence to:

  ```text
  EC -> DATA -> VID
  ```

- Treat `VID` as the only commit-visible record that makes a new mapping live.

### Fix init classification of "free" versus "uncommitted"

Current init logic should not assume that:

```text
EC valid + VID erased == free
```

That rule is unsafe once the write order becomes `EC -> DATA -> VID`, because a power cut after DATA and before VID leaves:

```text
EC valid + VID erased + data present
```

This is **not free**. It is an interrupted write and must be reclaimed through erase.

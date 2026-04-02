# Roadmap

Planned features and improvements for UBI on Zephyr. Items are listed by priority.

## Feature Overview

| Feature | Priority | Status | Description |
|---------|----------|--------|-------------|
| Crypto layer (authenticated encryption) | High | Design | AES-128-CCM encryption of all on-flash structures via PSA Crypto |
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

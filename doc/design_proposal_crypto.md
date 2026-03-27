# Design Proposal: UBI Crypto Layer — Authenticated Encryption for Flash

| Field          | Value                                         |
|----------------|-----------------------------------------------|
| **Author**     | Kamil Kielbasa                                |
| **Status**     | Draft                                         |
| **Created**    | 2026-03-27                                    |
| **Target**     | UBI subsystem on Zephyr RTOS                  |
| **Audience**   | Engineering team, security reviewers          |

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Problem Statement and Threat Model](#2-problem-statement-and-threat-model)
3. [Current State](#3-current-state)
4. [Architecture Overview](#4-architecture-overview)
5. [Header Structure Changes](#5-header-structure-changes)
6. [IV and Nonce Strategy](#6-iv-and-nonce-strategy)
7. [Sequence Number Notification Mechanism](#7-sequence-number-notification-mechanism)
8. [Key Rotation](#8-key-rotation)
9. [Crypto Module API](#9-crypto-module-api)
10. [External AAD Callback](#10-external-aad-callback)
11. [Integration Points](#11-integration-points)
12. [Kconfig Options](#12-kconfig-options)
13. [Testing Strategy](#13-testing-strategy)
14. [Phased Implementation Roadmap](#14-phased-implementation-roadmap)
15. [Open Questions](#15-open-questions)

---

## 1. Executive Summary

This proposal adds an optional authenticated encryption layer to UBI,
controlled by `CONFIG_UBI_CRYPTO`. When enabled, all on-flash structures
(device headers, volume headers, EC headers, VID headers, and LEB data
payloads) are encrypted and authenticated using **AES-128-CCM** via the
**PSA Crypto API**.

The design provides:

- **Confidentiality** — flash contents are ciphertext; dumping the chip
  reveals nothing.
- **Authenticity** — every header and data block carries a 16-byte
  auth tag; tampering is detected.
- **Location binding** — ESSIV-based nonces tie EC, VID, and data blocks
  to their physical flash location; relocating a PEB is detected.
- **Sequence number notification** — a configurable lazy callback
  notifies the application of the current sequence number every N
  writes; the application can persist it and implement anti-rollback
  externally.
- **Key rotation** — an online PEB-by-PEB re-encryption API allows
  migrating to a new key without data loss.

All crypto is conditional. With `CONFIG_UBI_CRYPTO=n` (default), the
on-flash format and all APIs remain unchanged.

---

## 2. Problem Statement and Threat Model

### Threats

| # | Threat | Description |
|---|--------|-------------|
| T1 | Flash dump | Attacker reads raw flash via JTAG/SWD or chip-off; extracts metadata and user data. |
| T2 | Flash tampering | Attacker modifies bytes on flash (e.g. via debug probe) to corrupt or inject data. |
| T3 | PEB relocation | Attacker copies a valid encrypted PEB to a different physical location to bypass wear-leveling or inject old data. |
| T4 | Partition rollback | Attacker reflashes the entire partition with an older valid snapshot to revert firmware state or data. |
| T5 | Volume injection | Attacker creates or modifies volume headers to add unauthorized volumes or alter volume properties. |
| T6 | Data replay | Attacker replays an old LEB write by restoring a previous PEB image for the same (vol_id, leb_num). |

### Out of Scope

- Physical side-channel attacks (power analysis, EM emanation).
- Key extraction from the PSA keystore (assumed tamper-resistant).
- Secure boot chain verification (separate concern).
- Key provisioning (application responsibility).
- DoS via flash wear-out (attacker with physical access can always destroy hardware).

### Known Limitations

- **Plaintext magic field**: The `magic` field in each header remains
  unencrypted so UBI can identify header type before decryption (required
  for ESSIV derivation and error classification). An attacker reading raw
  flash can identify header types and infer partition structure (number of
  PEBs, reserved PEB locations). This does not reveal user data or enable
  tampering.

---

## 3. Current State

UBI currently provides **integrity** via CRC-32 on all header structures.
There is no encryption, no authentication, and no anti-rollback.

```
Current on-flash layout (no crypto):

Reserved PEB:    [dev_hdr(32B) | vol_hdr_0(48B) | vol_hdr_1(48B) | ...]
Data PEB:        [ec_hdr(16B) | vid_hdr(32B) | user_data(leb_size)]

     All bytes are plaintext. CRC-32 detects accidental corruption
     but not intentional tampering.
```

Sequence numbers (`sqnum` in VID headers) exist and are monotonically
incremented on every LEB write. However:

- They are not persisted to any secure storage between reboots.
- No comparison is made at init to detect rollback.
- An attacker can reflash an older partition and UBI will accept it.

### 3.1 Encrypted On-Flash Layout

When `CONFIG_UBI_CRYPTO` is enabled, the on-flash layout changes as
follows. Every header grows to include an IV (12 B), auth tag (16 B),
and alignment padding. LEB data is encrypted in place with its auth
tag stored in the VID header.

#### Full Partition Layout (Encrypted)

```
+======================================================================+
|                       UBI FLASH PARTITION (encrypted)                 |
+======================================================================+
|                                                                      |
| Reserved PEB 0 (bank A):                                             |
| +------------------------------------------------------------------+ |
| | dev_hdr (64 B)                                                   | |
| | vol_hdr[0] (80 B) | vol_hdr[1] (80 B) | ... | vol_hdr[N] (80 B)| |
| +------------------------------------------------------------------+ |
|                                                                      |
| Reserved PEB 1 (bank B):  (mirror of bank A)                        |
| +------------------------------------------------------------------+ |
| | dev_hdr (64 B)                                                   | |
| | vol_hdr[0] (80 B) | vol_hdr[1] (80 B) | ... | vol_hdr[N] (80 B)| |
| +------------------------------------------------------------------+ |
|                                                                      |
| Data PEB 2:                                                          |
| +------------------------------------------------------------------+ |
| | ec_hdr (48 B) | vid_hdr (80 B) | LEB data (leb_size B)         | |
| +------------------------------------------------------------------+ |
|                                                                      |
| Data PEB 3:                                                          |
| +------------------------------------------------------------------+ |
| | ec_hdr (48 B) | vid_hdr (80 B) | LEB data (leb_size B)         | |
| +------------------------------------------------------------------+ |
|                                                                      |
|  ...                                                                 |
|                                                                      |
| Data PEB N-1:                                                        |
| +------------------------------------------------------------------+ |
| | ec_hdr (48 B) | vid_hdr (80 B) | LEB data (leb_size B)         | |
| +------------------------------------------------------------------+ |
+======================================================================+

Where:  leb_size = erase_block_size - 48 (ec_hdr) - 80 (vid_hdr)
                 = erase_block_size - 128
```

#### EC Header On-Flash (48 B)

```
Byte offset:
 0                   4        5     8       12      16              28     32              48
 +-------------------+--------+-----+-------+-------+---------------+------+---------------+
 | magic (4 B)       |ver (1B)|pad  | ec    |hdr_crc| iv (12 B)     | auth_tag (16 B)     |
 | 0x55424923        |  0x01  |(3B) | (4 B) | (4 B) | [nonce]       | [AES-CCM tag]       |
 +-------------------+--------+-----+-------+-------+---------------+------+------ --------+
 |<-- plaintext (8 B) ------------>||<-- encrypted (8 B) -->|<-- plaintext (crypto fields) -->|
                                     \_____ciphertext______/
```

- **Plaintext preamble**: `magic` (4 B) + `version` (1 B) + `padding` (3 B) = 8 B
- **Encrypted zone**: `ec` (4 B) + `hdr_crc` (4 B) = 8 B of ciphertext
- **IV**: 12 B nonce (ESSIV-derived from `peb_idx`)
- **Auth tag**: 16 B AES-CCM authentication tag
- **Padding**: 4 B alignment to 48 B total

#### VID Header On-Flash (80 B)

```
Byte offset:
 0           4     5     8       12      16      24          28      32
 +-----------+-----+-----+-------+-------+-------+-----------+-------+
 | magic     |ver  |pad  | lnum  |vol_id | sqnum (8 B)      |d_size |
 | 0x55424921|(1B) |(3B) | (4 B) | (4 B) |                   | (4 B) |
 +-----------+-----+-----+-------+-------+-------------------+-------+
 |<- plaintext (8 B) -->||<---------- encrypted (24 B) ------------->|

 32          36              48              64              80
 +-------+--+---------------+---------------+---------------+
 |hdr_crc|  | iv (12 B)     | auth_tag      | data_tag      |
 | (4 B) |  | [nonce]       | (16 B)        | (16 B)        |
 +-------+--+---------------+---------------+---------------+
 |<-enc->|  |<------------ plaintext (crypto fields) ------>|

 Total encrypted zone: lnum(4) + vol_id(4) + sqnum(8) + data_size(4) + hdr_crc(4) = 24 B
```

- **Plaintext preamble**: `magic` (4 B) + `version` (1 B) + `padding` (3 B) = 8 B
- **Encrypted zone**: `lnum` + `vol_id` + `sqnum` + `data_size` + `hdr_crc` = 24 B
- **IV**: 12 B nonce (ESSIV-derived from `peb_idx` || `ec`)
- **Auth tag**: 16 B AES-CCM tag for the VID header itself
- **Data tag**: 16 B AES-CCM tag for the associated LEB data payload
- **Padding**: 4 B alignment to 80 B total

#### Device Header On-Flash (64 B)

```
Byte offset:
 0           4     5     8       12      16      20      24      28      32
 +-----------+-----+-----+-------+-------+-------+-------+-------+-------+
 | magic     |ver  |pad  |offset | size  |revis. |vol_cnt|pad_2  |hdr_crc|
 | 0x55424925|(1B) |(3B) | (4 B) | (4 B) | (4 B) | (4 B) | (4 B) | (4 B) |
 +-----------+-----+-----+-------+-------+-------+-------+-------+-------+
 |<- plaintext (8 B) -->||<------------- encrypted (24 B) -------------->|

 32              44              48              64
 +---------------+---+-----------+---------------+
 | iv (12 B)     |   | auth_tag (16 B)           |
 | [random]      |   | [AES-CCM tag]             |
 +---------------+---+---------------------------+
 |<---------- plaintext (crypto fields) -------->|
```

- **Plaintext preamble**: `magic` (4 B) + `version` (1 B) + `padding` (3 B) = 8 B
- **Encrypted zone**: `offset` + `size` + `revision` + `vol_count` + `padding_2` + `hdr_crc` = 24 B
- **IV**: 12 B random nonce (CSPRNG — not ESSIV; mirrored across reserved PEB banks)
- **Auth tag**: 16 B AES-CCM tag
- **Padding**: 4 B alignment to 64 B total

#### Volume Header On-Flash (80 B)

```
Byte offset:
 0           4     5  6     8       12      16                  32
 +-----------+-----+--+-----+-------+-------+-------------------+
 | magic     |ver  |vt|pad  |vol_id |leb_cnt| padding_2 (12 B) |
 | 0x55424926|(1B) |  |(2B) | (4 B) | (4 B) |                   |
 +-----------+-----+--+-----+-------+-------+-------------------+

 32                                  48      52              64              80
 +-----------------------------------+-------+---------------+---------------+
 | name (16 B)                       |hdr_crc| iv (12 B)     | auth_tag      |
 |                                   | (4 B) | [random]      | (16 B)        |
 +-----------------------------------+-------+---------------+---------------+
 |<- plaintext (8 B) -->|<------ encrypted (40 B) ------>|<- crypto fields ->|
```

- **Plaintext preamble**: `magic` (4 B) + `version` (1 B) + `vol_type` (1 B) + `padding` (2 B) = 8 B
- **Encrypted zone**: `vol_id` + `leb_count` + `padding_2` + `name` + `hdr_crc` = 40 B
- **IV**: 12 B random nonce (CSPRNG — same rationale as device header)
- **Auth tag**: 16 B AES-CCM tag
- **Padding**: 4 B alignment to 80 B total

#### LEB Data On-Flash

LEB data is encrypted in place. The auth tag is **not** stored
alongside the data — it lives in the VID header's `data_tag` field.

```
                      Data PEB (encrypted)
 +--------+----------+-----------------------------------------------+
 | ec_hdr | vid_hdr  | LEB data (encrypted)                         |
 | (48 B) | (80 B)   | (leb_size B of AES-CCM ciphertext)           |
 +--------+----------+-----------------------------------------------+
 |                    |                                               |
 |                    |  vid_hdr.data_tag authenticates this region   |
 |                    |  vid_hdr.iv is used as the LEB data nonce     |
 |                    |  (ESSIV-derived from peb_idx, leb_num,        |
 |                    |   vol_id, sqnum)                              |
 +--------+----------+-----------------------------------------------+

 On flash, the data region contains only ciphertext — no inline IV
 or tag. Both are in the VID header, keeping the data region the same
 size as without crypto.
```

---

## 4. Architecture Overview

### Layered Position

The crypto layer sits between the existing UBI I/O functions and the
Zephyr Flash Area API:

```
+-----------------------------------------------------+
|                   Application                        |
+-----------------------------------------------------+
            |                          ^
            v                          |
+-----------------------------------------------------+
|                     UBI Layer                        |
|  Volume Mgmt | LEB I/O | Wear-Level | Reserved PEB  |
+-----------------------------------------------------+
            |                          ^
            | ubi_ec_hdr_write()       | ubi_ec_hdr_read()
            | ubi_vid_hdr_write()      | ubi_vid_hdr_read()
            | ubi_leb_data_write()     | ubi_leb_data_read()
            v                          |
+-----------------------------------------------------+
|              UBI Crypto Layer (NEW)                  |
|                                                      |
|  +---------------+  +-----------------------------+  |
|  | seal / open   |  | ESSIV nonce derivation      |  |
|  | (AES-128-CCM) |  | Sqnum notification (cb)     |  |
|  +---------------+  | Key rotation (rekey)        |  |
|                      +-----------------------------+  |
|                                                      |
|  PSA Crypto API  (psa_aead_encrypt / decrypt)        |
+-----------------------------------------------------+
            |                          ^
            | flash_area_write()       | flash_area_read()
            v                          |
+-----------------------------------------------------+
|          Zephyr Flash Area API (Flash Map)           |
+-----------------------------------------------------+
```

### Encrypt / Decrypt Flow

```
WRITE PATH:
  fill struct --> compute CRC (over plaintext) --> encrypt --> flash_area_write
                                                     |
                                              [iv | ciphertext | tag]

READ PATH:
  flash_area_read --> decrypt --> verify CRC (over plaintext) --> use struct
                        |
                   [iv | ciphertext | tag]

  CRC is inside the ciphertext. Auth tag provides cryptographic
  integrity. CRC provides backward-compatible structural validation
  after decryption.
```

### Encrypted Header Layout (generic)

Every header follows this layout when crypto is enabled:

```
+--------------------+----------------------------------+-----------+
| Plaintext Preamble |       Encrypted Zone             | Auth tag  |
| (readable without  |   (AES-CCM ciphertext over       | (16 B)    |
|  decryption)       |    original fields + CRC)        |           |
+--------------------+----------------------------------+-----------+
| magic(4) + iv(12)  | [original fields minus magic,    | tag(16)   |
|                     |  padded to 16B boundary]         |           |
+--------------------+----------------------------------+-----------+
      AAD input                Ciphertext                  Tag
```

The `magic` field remains plaintext so UBI can identify header type
before decryption (required for ESSIV derivation and error classification).
The IV is stored in the header itself — no external state needed for
decryption.

---

## 5. Header Structure Changes

All changes are conditional on `CONFIG_UBI_CRYPTO`. Without it, structs
and sizes remain identical to current values.

### 5.1 Size Summary

| Header       | Current | + IV  | + Tag | + Data Tag | + Pad | Crypto Size | Delta |
|--------------|---------|-------|-------|------------|-------|-------------|-------|
| `ubi_ec_hdr` | 16 B    | +12 B | +16 B | —          | +4 B  | 48 B        | +32 B |
| `ubi_vid_hdr`| 32 B    | +12 B | +16 B | +16 B      | +4 B  | 80 B        | +48 B |
| `ubi_dev_hdr`| 32 B    | +12 B | +16 B | —          | +4 B  | 64 B        | +32 B |
| `ubi_vol_hdr`| 48 B    | +12 B | +16 B | —          | +4 B  | 80 B        | +32 B |

IV is 12 bytes (AES-CCM with 12-byte nonce, `q = 3`, max message
2^24 − 1 bytes which is sufficient for flash erase blocks up to 16 MB).
Auth tag is 16 bytes (full CCM tag). The VID header carries an
additional 16-byte `data_tag` for the LEB data payload (see 5.5).
Padding aligns to 16-byte write-block boundary.

### 5.2 Struct Layout (example: `ubi_ec_hdr`)

```c
struct ubi_ec_hdr {
    uint32_t magic;             /* 4 B  — plaintext */
    uint8_t  version;           /* 1 B  — plaintext */
    uint8_t  padding[3];        /* 3 B  — plaintext */
    uint32_t ec;                /* 4 B  — encrypted */
    uint32_t hdr_crc;           /* 4 B  — encrypted (CRC of plaintext) */
#if defined(CONFIG_UBI_CRYPTO)
    uint8_t  iv[12];            /* 12 B — plaintext (nonce for AES-CCM) */
    uint8_t  auth_tag[16];      /* 16 B — auth tag */
    uint8_t  pad_crypto[4];     /* 4 B  — alignment to 48 B */
#endif
};
```

### 5.3 Conditional Size Macros

```c
#if defined(CONFIG_UBI_CRYPTO)
#define UBI_CRYPTO_IV_LEN   (12)  /* Intentional: 12-byte nonce (q=3) for >64KB blocks */
#define UBI_CRYPTO_TAG_LEN  PSA_AEAD_TAG_LENGTH(PSA_KEY_TYPE_AES, 128, PSA_ALG_CCM)
#define UBI_EC_HDR_SIZE     (48)
#define UBI_VID_HDR_SIZE    (80)
#define UBI_DEV_HDR_SIZE    (64)
#define UBI_VOL_HDR_SIZE    (80)

BUILD_ASSERT(UBI_CRYPTO_TAG_LEN == 16, "Unexpected AES-CCM tag length");
BUILD_ASSERT(UBI_CRYPTO_IV_LEN == 12, "AES-CCM nonce must be 12 bytes for q=3");
#else
#define UBI_EC_HDR_SIZE     (16)
#define UBI_VID_HDR_SIZE    (32)
#define UBI_DEV_HDR_SIZE    (32)
#define UBI_VOL_HDR_SIZE    (48)
#endif
```

### 5.4 LEB Size Impact

```
leb_size = erase_block_size - UBI_EC_HDR_SIZE - UBI_VID_HDR_SIZE

Without crypto:  leb_size = erase_block_size - 16 - 32  = erase_block_size - 48
With crypto:     leb_size = erase_block_size - 48 - 80  = erase_block_size - 128

Delta: -80 bytes per LEB
```

For a typical 4 KB erase block: 4048 B → 3968 B (2.0% reduction).

### 5.5 VID Header: Data Auth Tag

When crypto is enabled, the VID header carries an additional field for the
LEB data payload auth tag. This binds data integrity to the VID
header without changing the data region layout:

```c
struct ubi_vid_hdr {
    /* ... existing fields ... */
#if defined(CONFIG_UBI_CRYPTO)
    uint8_t  iv[12];             /* 12 B — nonce for AES-CCM */
    uint8_t  auth_tag[16];       /* 16 B — auth tag for VID header */
    uint8_t  data_tag[16];       /* 16 B — auth tag for LEB data payload */
    uint8_t  pad_crypto[4];
    /* Total: 32 + 12 + 16 + 16 + 4 = 80 B */
#endif
};
```

The `data_tag` binds LEB data integrity to its VID header. If the VID
header is corrupted, the LEB data payload becomes unverifiable.

---

## 6. IV and Nonce Strategy

### 6.1 Per-Header IV Source

| Header     | IV Source | Inputs                               | Rationale |
|------------|----------|--------------------------------------|-----------|
| `dev_hdr`  | Random   | 12 B from CSPRNG                     | Mirrored on N reserved PEBs; ESSIV would produce identical ciphertext across banks |
| `vol_hdr`  | Random   | 12 B from CSPRNG                     | Same as dev_hdr — stored on reserved PEBs |
| `ec_hdr`   | ESSIV    | `0x01 \|\| peb_idx`                  | Domain-separated; ties EC header to physical location; prevents relocation (T3) |
| `vid_hdr`  | ESSIV    | `0x02 \|\| peb_idx \|\| ec`           | Domain-separated; EC gives freshness per erase cycle; prevents replay after erase (T6) |
| LEB data   | ESSIV    | `0x03 \|\| peb_idx \|\| leb_num \|\| vol_id \|\| sqnum(64-bit)` | Domain-separated; full sqnum gives per-write freshness (T6) |

### 6.2 ESSIV Construction

ESSIV (Encrypted Salt-Sector IV) derives a deterministic nonce from
context data, preventing IV reuse while binding ciphertext to its
physical location.

#### Key Derivation

UBI derives two purpose-specific subkeys from the application-provided
master key at `ubi_crypto_init()` time using HKDF (PSA key derivation):

```
K_enc   = HKDF-Expand(K_main, "ubi-aes-ccm-enc", 16)   — used for all AES-CCM operations
K_essiv = HKDF-Expand(K_main, "ubi-essiv-key",   16)   — used for IV derivation
```

This provides cryptographic domain separation between encryption and
nonce generation per NIST SP 800-57 key management guidelines.

#### IV Computation

```
IV = AES-128-ECB-Encrypt(K_essiv, input_block)[0:12]
```

K_essiv is derived via HKDF (see Key Derivation above), analogous in
purpose to the IEEE P1619.1 / dm-crypt ESSIV approach but using a
modern key derivation function instead of raw SHA-256 truncation.

#### Input Blocks with Domain Separators

Each header type has a unique 1-byte domain separator as the first byte
of the 16-byte input block. This guarantees that no two header types
can produce the same ESSIV input regardless of field values:

```
EC header:   [ 0x01 | peb_idx(4) | 0x00(11)                         ]
VID header:  [ 0x02 | peb_idx(4) | ec(4) | 0x00(7)                   ]
LEB data:    [ 0x03 | peb_idx(3) | leb_num(2) | vol_id(2) | sqnum(8) ]
```

Note: LEB data uses the full 64-bit sqnum for nonce freshness. The
peb_idx, leb_num, and vol_id fields are truncated to fit within the
16-byte block (peb_idx: max 16M PEBs, leb_num: max 64K, vol_id:
max 64K).

### 6.3 AAD Construction

Additional Authenticated Data binds ciphertext identity without
encrypting the binding context:

| Header     | AAD Contents                                           |
|------------|--------------------------------------------------------|
| `dev_hdr`  | `magic(4) \|\| version(1) \|\| 0x00(3)`               |
| `vol_hdr`  | `magic(4) \|\| version(1) \|\| vol_index(1) \|\| 0x00(2)` |
| `ec_hdr`   | `magic(4) \|\| version(1) \|\| peb_idx(3)`            |
| `vid_hdr`  | `magic(4) \|\| version(1) \|\| peb_idx(3)`            |
| LEB data   | `ec(4) \|\| peb_idx(4) \|\| leb_num(4) \|\| vol_id(4) \|\| sqnum(8) \|\| data_size(4)` + optional user AAD |

### 6.4 Step-by-Step: Encrypting an EC Header (seal)

```
Given:  K_main (application key), peb_idx = 5, ec = 42

STEP 1 — Derive subkeys (once at ubi_crypto_init):
  K_enc   = HKDF-Expand(K_main, "ubi-aes-ccm-enc", 16)
  K_essiv = HKDF-Expand(K_main, "ubi-essiv-key",   16)

STEP 2 — Fill plaintext struct:
  ec_hdr.magic   = 0x55424923
  ec_hdr.version = 0x01
  ec_hdr.padding = {0, 0, 0}
  ec_hdr.ec      = 42

STEP 3 — Compute CRC over plaintext fields:
  ec_hdr.hdr_crc = CRC32(magic || version || padding || ec)

STEP 4 — Compute ESSIV nonce (deterministic IV):
  input_block = [ 0x01 | 0x00000005 | 0x00 * 11 ]   (16 bytes)
                  ^       ^
                  |       peb_idx (big-endian, 4 B)
                  domain separator for EC header
  iv = AES-128-ECB-Encrypt(K_essiv, input_block)[0:12]

STEP 5 — Prepare plaintext for encryption:
  plaintext = ec(4 B) || hdr_crc(4 B) = 8 bytes

STEP 6 — Build AAD:
  aad = magic(4 B) || version(1 B) || peb_idx(3 B) = 8 bytes

STEP 7 — Encrypt with AES-128-CCM:
  (ciphertext, tag) = psa_aead_encrypt(
      key       = K_enc,
      alg       = PSA_ALG_CCM,
      nonce     = iv (12 B from step 4),
      aad       = aad (8 B from step 6),
      plaintext = plaintext (8 B from step 5)
  )
  ciphertext = 8 bytes,  tag = 16 bytes

STEP 8 — Assemble on-flash layout (48 B):
  +----------+-----+-----+--------------+--------+-----------+------+
  | magic    |ver  |pad  | ciphertext   | iv     | auth_tag  | pad  |
  | (4 B)    |(1B) |(3B) | (8 B)        | (12 B) | (16 B)    | (4B) |
  +----------+-----+-----+--------------+--------+-----------+------+
  0          4     5     8              16       28          44    48

STEP 9 — Write to flash:
  flash_area_write(fa, peb_offset, &ec_hdr, 48)
```

### 6.5 Step-by-Step: Decrypting an EC Header (open)

```
Given:  K_enc, K_essiv (derived at init), peb_idx = 5

STEP 1 — Read from flash:
  flash_area_read(fa, peb_offset, &ec_hdr, 48)

STEP 2 — Verify magic:
  if ec_hdr.magic != 0x55424923 → not an EC header, return error

STEP 3 — Recompute ESSIV nonce:
  input_block = [ 0x01 | 0x00000005 | 0x00 * 11 ]
  iv = AES-128-ECB-Encrypt(K_essiv, input_block)[0:12]
  (Same as seal step 4 — same peb_idx produces same IV)

STEP 4 — Rebuild AAD:
  aad = magic(4 B) || version(1 B) || peb_idx(3 B) = 8 bytes

STEP 5 — Extract ciphertext and tag from on-flash layout:
  ciphertext = bytes [8..15]   (8 B)
  tag        = bytes [28..43]  (16 B)

STEP 6 — Decrypt + authenticate with AES-128-CCM:
  plaintext = psa_aead_decrypt(
      key        = K_enc,
      alg        = PSA_ALG_CCM,
      nonce      = iv (12 B from step 3),
      aad        = aad (8 B from step 4),
      ciphertext = ciphertext (8 B),
      tag        = tag (16 B)
  )
  If auth fails → return -EBADMSG (tampered or wrong PEB location)

STEP 7 — Restore plaintext fields:
  ec_hdr.ec      = plaintext[0..3]
  ec_hdr.hdr_crc = plaintext[4..7]

STEP 8 — Verify CRC:
  expected = CRC32(magic || version || padding || ec)
  if ec_hdr.hdr_crc != expected → return -EBADMSG
```

### 6.6 Step-by-Step: Encrypting a VID Header + LEB Data (seal)

```
Given:  K_enc, K_essiv, peb_idx = 5, ec = 42,
        vol_id = 1, lnum = 3, sqnum = 1000, data = <user bytes>, data_size = 512

STEP 1 — Fill plaintext VID header:
  vid_hdr.magic     = 0x55424921
  vid_hdr.version   = 0x01
  vid_hdr.lnum      = 3
  vid_hdr.vol_id    = 1
  vid_hdr.sqnum     = 1000
  vid_hdr.data_size = 512
  vid_hdr.hdr_crc   = CRC32(all plaintext VID fields)

== Encrypt LEB data first (tag goes into VID header) ==

STEP 2 — Compute LEB data ESSIV nonce:
  input_block = [ 0x03 | peb_idx(3B) | leb_num(2B) | vol_id(2B) | sqnum(8B) ]
              = [ 0x03 | 0x000005    | 0x0003      | 0x0001     | 0x00000000000003E8 ]
  iv_data = AES-128-ECB-Encrypt(K_essiv, input_block)[0:12]

STEP 3 — Build LEB data AAD:
  aad_data = ec(4B) || peb_idx(4B) || leb_num(4B) || vol_id(4B)
             || sqnum(8B) || data_size(4B)
           = 28 bytes  (+ optional external AAD from callback)

STEP 4 — Encrypt LEB data:
  (ciphertext_data, tag_data) = psa_aead_encrypt(
      key       = K_enc,
      nonce     = iv_data,
      aad       = aad_data,
      plaintext = user_data (512 B)
  )
  Store tag_data → vid_hdr.data_tag (16 B)

== Now encrypt VID header ==

STEP 5 — Compute VID header ESSIV nonce:
  input_block = [ 0x02 | 0x00000005 | 0x0000002A | 0x00 * 7 ]
                  ^       ^            ^
                  |       peb_idx      ec (= 42 = 0x2A)
                  domain separator for VID header
  iv_vid = AES-128-ECB-Encrypt(K_essiv, input_block)[0:12]

STEP 6 — Build VID header AAD:
  aad_vid = magic(4B) || version(1B) || peb_idx(3B) = 8 bytes

STEP 7 — Encrypt VID header fields:
  plaintext_vid = lnum(4B) || vol_id(4B) || sqnum(8B)
                  || data_size(4B) || hdr_crc(4B) = 24 bytes
  (ciphertext_vid, tag_vid) = psa_aead_encrypt(
      key       = K_enc,
      nonce     = iv_vid,
      aad       = aad_vid,
      plaintext = plaintext_vid
  )

STEP 8 — Assemble on-flash VID header (80 B):
  +--------+-----+-----+--------------------+--------+--------+----------+----------+-----+
  | magic  |ver  |pad  | ciphertext_vid     |        | iv_vid | auth_tag | data_tag | pad |
  | (4 B)  |(1B) |(3B) | (24 B)             |        | (12 B) | (16 B)   | (16 B)   |(4B) |
  +--------+-----+-----+--------------------+--------+--------+----------+----------+-----+
  0        4     5     8                    32       36       48         64         80

STEP 9 — Write to flash:
  flash_area_write(fa, peb_offset + 48, &vid_hdr, 80)   // after ec_hdr
  flash_area_write(fa, peb_offset + 128, ciphertext_data, 512)
```

### 6.7 Step-by-Step: Decrypting LEB Data (open)

```
Given:  K_enc, K_essiv, peb_idx = 5

STEP 1 — Read + decrypt VID header (steps from 6.5, adapted for VID):
  → Obtain: lnum, vol_id, sqnum, data_size, data_tag, iv fields

STEP 2 — Recompute LEB data ESSIV nonce:
  input_block = [ 0x03 | peb_idx(3B) | leb_num(2B) | vol_id(2B) | sqnum(8B) ]
  iv_data = AES-128-ECB-Encrypt(K_essiv, input_block)[0:12]

STEP 3 — Rebuild LEB data AAD:
  aad_data = ec(4B) || peb_idx(4B) || leb_num(4B) || vol_id(4B)
             || sqnum(8B) || data_size(4B)

STEP 4 — Read encrypted LEB data from flash:
  flash_area_read(fa, peb_offset + 128, ciphertext_data, data_size)

STEP 5 — Decrypt + authenticate:
  plaintext = psa_aead_decrypt(
      key        = K_enc,
      nonce      = iv_data,
      aad        = aad_data,
      ciphertext = ciphertext_data,
      tag        = vid_hdr.data_tag (16 B)
  )
  If auth fails → return -EBADMSG
  (Detects: bit-flip, PEB relocation, LEB replay, volume injection)

STEP 6 — Return plaintext to caller
```

### 6.8 Step-by-Step: Encrypting Device / Volume Headers (seal)

```
Device and volume headers use RANDOM IVs (not ESSIV) because they
are mirrored across multiple reserved PEBs. Using ESSIV would produce
identical ciphertext across banks — a random IV ensures each bank
has unique ciphertext.

Given:  K_enc, device header with filled plaintext fields

STEP 1 — Compute CRC over plaintext fields:
  dev_hdr.hdr_crc = CRC32(all plaintext dev_hdr fields)

STEP 2 — Generate random IV:
  iv = psa_generate_random(12)   // CSPRNG, 12 bytes

STEP 3 — Build AAD:
  aad = magic(4B) || version(1B) || 0x00(3B) = 8 bytes

STEP 4 — Encrypt:
  plaintext = offset(4B) || size(4B) || revision(4B)
              || vol_count(4B) || padding_2(4B) || hdr_crc(4B) = 24 bytes
  (ciphertext, tag) = psa_aead_encrypt(K_enc, PSA_ALG_CCM,
                                         iv, aad, plaintext)

STEP 5 — Assemble on-flash (64 B) and write to flash

Volume headers follow the same pattern:
  - Random IV (12 B from CSPRNG)
  - AAD = magic(4B) || version(1B) || vol_index(1B) || 0x00(2B)
  - Encrypted zone = vol_id + leb_count + padding_2 + name + hdr_crc
```

---

## 7. Sequence Number Notification Mechanism

### 7.1 Design Philosophy

UBI does **not** implement anti-rollback detection internally. Instead,
UBI exposes its monotonic sequence number (`global_sqnum`) and provides
a configurable **lazy notification callback**. The application reads the
sequence number from UBI and decides what to do — persist it to secure
storage, compare it against a trusted reference, trigger an alert, etc.

This separation keeps UBI focused on flash management while giving
applications full control over their security policy.

### 7.2 Sequence Number Lifecycle

```
                    ubi_device_init()
                          |
                          v
              +------------------------+
              | init_scan_pebs()       |  Scan all VID headers,
              | --> global_sqnum       |  find max sqnum on flash
              +------------------------+
                          |
                          v
                   Normal operation
                   (sqnum available via
                    ubi_device_get_info)
                          |
              (every LEB write: sqnum++)
                          |
                          v
              +-------------------------------+
              | write_count - last_notified   |  YES --> sqnum_notify_cb(sqnum, user_data)
              | >= sqnum_notify_delta ?        |          update last_notified
              +-------------------------------+
                          |
                          v
              +-------------------------------+
              | ubi_device_deinit()           |  sqnum_notify_cb(sqnum, user_data)
              |                               |  final notification
              +-------------------------------+
```

### 7.3 Notification Callback

```c
/**
 * Notify application about the current sequence number.
 *
 * UBI calls this callback every `sqnum_notify_delta` LEB writes and
 * once during ubi_device_deinit(). The application decides what to
 * do with the sqnum: persist to secure storage, compare against a
 * reference, log, etc.
 *
 * @param sqnum      Current global sequence number.
 * @param user_data  Opaque pointer passed through from configuration.
 * @return 0 on success, negative error code on failure.
 *         If the callback returns an error, UBI propagates it to the
 *         caller (LEB write fails with the same error code).
 */
typedef int (*ubi_crypto_sqnum_notify_cb)(uint64_t sqnum, void *user_data);
```

**Key points:**
- UBI does **not** read any persisted sqnum at init.
- UBI does **not** compare sqnum values or make rollback decisions.
- The application receives periodic sqnum notifications and is free
  to implement any policy (or none at all).

### 7.4 Configuration

```c
struct ubi_crypto_cfg {
    psa_key_id_t key_id;

#if defined(CONFIG_UBI_CRYPTO_SQNUM_NOTIFY)
    ubi_crypto_sqnum_notify_cb sqnum_notify;  /* Called every N writes */
    void                      *sqnum_user_data; /* Opaque app context */
    size_t                     sqnum_notify_delta; /* N writes between calls */
#endif

    /* ... other fields ... */
};
```

- `CONFIG_UBI_CRYPTO_SQNUM_NOTIFY` — enable the notification mechanism.
- `sqnum_notify_delta` — number of LEB writes between callback
  invocations (e.g. 1000). The callback is also called during
  `ubi_device_deinit()` regardless of the delta counter.

### 7.5 Delta Selection Guidance

| Delta | Use Case | Callback freq at 1 write/sec |
|-------|----------|------------------------------|
| 1     | Every write (maximum granularity) | 1 call/sec |
| 100   | Balanced (typical IoT applications) | 1 call every ~2 min |
| 1000  | High-write workloads (sensor logging) | 1 call every ~17 min |

**Warning**: `delta = 1` invokes the callback on every LEB write.
If the callback writes to secure storage (OTP, eFuse), consider
the wear tolerance of that storage.

### 7.6 Application-Side Anti-Rollback Example

The application can implement anti-rollback on top of this mechanism:

```c
/* Application code — NOT inside UBI */

static uint64_t persisted_sqnum;

static int my_sqnum_notify(uint64_t sqnum, void *user_data)
{
    /* Persist sqnum to secure storage (e.g. TrustZone, eFuse). */
    return secure_storage_write("ubi_sqnum", &sqnum, sizeof(sqnum));
}

/* At application startup, BEFORE ubi_device_init(): */
int app_init(void)
{
    /* Read persisted sqnum from secure storage. */
    secure_storage_read("ubi_sqnum", &persisted_sqnum, sizeof(persisted_sqnum));

    /* Initialize UBI with notification callback. */
    struct ubi_crypto_cfg crypto_cfg = {
        .key_id             = my_key_id,
        .sqnum_notify       = my_sqnum_notify,
        .sqnum_user_data    = NULL,
        .sqnum_notify_delta = 1000,
    };

    struct ubi_mtd mtd = { .partition_id = 0, .crypto = &crypto_cfg, ... };
    struct ubi_device *ubi;
    int rc = ubi_device_init(&mtd, &ubi);
    if (rc) return rc;

    /* Check for rollback: compare persisted sqnum with flash sqnum. */
    struct ubi_device_info info;
    ubi_device_get_info(ubi, &info);

    if (info.global_sqnum < persisted_sqnum) {
        /* Rollback detected — application decides the policy. */
        LOG_ERR("Rollback detected: flash=%llu, persisted=%llu",
                info.global_sqnum, persisted_sqnum);
        ubi_device_deinit(ubi);
        return -EACCES;
    }

    return 0;
}
```

### 7.7 Public API

```c
/** Expose current sqnum in device info (always available with CONFIG_UBI_CRYPTO). */
struct ubi_device_info {
    /* ... existing fields ... */
    uint64_t global_sqnum;
};

/**
 * Force an immediate sqnum notification callback invocation.
 * Useful before sleep, after critical writes, or at application checkpoints.
 */
int ubi_crypto_notify_sqnum(struct ubi_device *ubi);
```

### 7.8 Kconfig

```kconfig
config UBI_CRYPTO_SQNUM_NOTIFY
    bool "Sequence number notification callback"
    depends on UBI_CRYPTO
    help
      Enable a configurable callback that UBI invokes every N LEB
      writes with the current global sequence number. The application
      can use this to persist the sqnum to secure storage and
      implement anti-rollback detection externally.
```

---

## 8. Key Rotation

### 8.1 Problem

Symmetric keys have a limited cryptoperiod. When the key must be changed
(policy expiry, suspected compromise), all on-flash data encrypted with
the old key must be re-encrypted with the new key without data loss.

### 8.2 Approach: Online PEB-by-PEB Re-Encryption

```
ubi_crypto_rekey(ubi, new_key_id)
          |
          v
+-------------------------------+
| 1. Write rotation state to    |  dev_hdr.rekey_state = IN_PROGRESS
|    reserved PEBs:             |  dev_hdr.old_key_id  = current
|    {IN_PROGRESS, old_key,     |  dev_hdr.new_key_id  = new_key_id
|     new_key, last_peb=0}      |  dev_hdr.rekey_peb   = 0
+-------------------------------+
          |
          v
+-------------------------------+
| 2. For each data PEB (2..N-1):|
|    a. Read + decrypt with     |  Uses old_key_id
|       old key                 |
|    b. Encrypt with new key    |  Uses new_key_id
|    c. Write re-encrypted data |  To scratch PEB (spare)
|       to scratch PEB          |
|    d. Erase original PEB      |  Old data gone, but copy exists
|    e. Copy from scratch to    |  Or remap scratch as new PEB
|       original PEB            |
|    f. Erase scratch PEB       |  Return to spare pool
|    g. Update rekey_peb in     |  Crash resume point
|       reserved PEBs           |
+-------------------------------+
          |
          v
+-------------------------------+
| 3. Re-encrypt reserved PEBs   |  Last step: dev_hdr + vol_hdrs
|    with new key                |  re-encrypted with new_key_id
+-------------------------------+
          |
          v
+-------------------------------+
| 4. Write final state:         |  dev_hdr.rekey_state = IDLE
|    {IDLE, new_key_id}         |  Commit point
+-------------------------------+
          |
          v
     Return success
     (app may now call psa_destroy_key(old_key_id))
```

### 8.3 Crash Safety

The rekey algorithm uses a **write-before-erase** strategy with a spare
PEB from the reserved pool as a scratch area. This ensures that data
always exists on flash — a power loss at any point cannot cause data
loss:

```
For each PEB:
  1. Read + decrypt from PEB_src (old key)
  2. Encrypt with new key
  3. Write to PEB_scratch (spare from reserved pool)
  4. Erase PEB_src                   — data safe on PEB_scratch
  5. Copy from PEB_scratch to PEB_src (or remap scratch as PEB_src)
  6. Erase PEB_scratch               — return to spare pool
  7. Update rekey_peb in reserved PEBs (crash resume point)
```

The rotation state is persisted in the device header on reserved PEBs:

```c
#if defined(CONFIG_UBI_CRYPTO_KEY_ROTATION)
struct ubi_rekey_state {
    uint8_t  state;         /* 0 = IDLE, 1 = IN_PROGRESS */
    uint8_t  padding[3];
    uint32_t old_key_id;    /* PSA key ID of old key */
    uint32_t new_key_id;    /* PSA key ID of new key */
    uint32_t last_peb;      /* Last successfully rekeyed PEB index */
};
#endif
```

On `ubi_device_init()`, if `rekey_state == IN_PROGRESS`:
1. Both keys must be present in the PSA keystore.
2. PEBs `0..last_peb` are encrypted with `new_key_id`.
3. PEBs `last_peb+1..N-1` are encrypted with `old_key_id`.
4. Check PEB `last_peb + 1` — if empty (crash during erase/write),
   the scratch PEB contains the re-encrypted copy. Recover from scratch.
5. Resume re-encryption from `last_peb + 1`.
6. Upon completion, write `IDLE` state.

### 8.4 Key Destruction Policy

**Critical**: The application MUST NOT call `psa_destroy_key(old_key_id)`
until `ubi_crypto_rekey()` returns success. If the old key is destroyed
while rotation is in progress, PEBs still encrypted with the old key are
permanently unrecoverable.

UBI itself never destroys keys — this is the application's responsibility.

### 8.5 Performance

Re-encryption is O(N) where N = total PEBs. Each PEB requires one
read, two writes (to scratch + back to original), and two erases
(original + scratch). For a 1 MB partition with 4 KB erase blocks
(256 PEBs), this is ~512 erase cycles — completing in seconds on typical
NOR flash.

The operation holds the UBI mutex for its duration (blocking other
operations). For large partitions, a future enhancement could allow
background re-encryption with per-PEB locking.

### 8.6 Reading During Rotation

During rotation, reads must determine which key to use per PEB. The
approach:
- PEBs `<= last_peb` → try `new_key_id` first.
- PEBs `> last_peb` → try `old_key_id` first.
- If decryption fails with the expected key, try the other (handles
  edge cases around the boundary PEB).

### 8.7 API

```c
/**
 * Re-encrypt all PEBs with a new key.
 *
 * Both old and new keys must be present in the PSA keystore.
 * Returns 0 on success. Do NOT destroy the old key until this returns.
 */
int ubi_crypto_rekey(struct ubi_device *ubi, psa_key_id_t new_key_id);
```

### 8.8 Kconfig

```
config UBI_CRYPTO_KEY_ROTATION
    bool "Enable key rotation support"
    depends on UBI_CRYPTO
    help
      Adds ubi_crypto_rekey() API for online re-encryption with a new key.
      Increases device header size by 16 bytes for rotation state tracking.
```

---

## 9. Crypto Module API

### 9.1 New Files

- `lib/src/ubi_crypto.h` — types, callback typedefs, function declarations.
- `lib/src/ubi_crypto.c` — PSA Crypto implementation.

### 9.2 Configuration Structure

```c
struct ubi_crypto_cfg {
    psa_key_id_t key_id;

#if defined(CONFIG_UBI_CRYPTO_SQNUM_NOTIFY)
    ubi_crypto_sqnum_notify_cb sqnum_notify;
    void                      *sqnum_user_data;
    size_t                     sqnum_notify_delta;
#endif

#if defined(CONFIG_UBI_CRYPTO_EXTERNAL_AAD)
    ubi_crypto_aad_cb         aad_cb;
#endif
};
```

Passed to UBI via `ubi_mtd`:

```c
struct ubi_mtd {
    uint8_t  partition_id;
    size_t   write_block_size;
    size_t   erase_block_size;
#if defined(CONFIG_UBI_CRYPTO)
    const struct ubi_crypto_cfg *crypto;
#endif
};
```

### 9.3 Function Declarations

```c
/* Lifecycle */
int  ubi_crypto_init(const struct ubi_crypto_cfg *cfg);
void ubi_crypto_deinit(void);
```

`ubi_crypto_init()` performs the following at startup:
1. Validate key attributes via `psa_get_key_attributes()`: type must be
   `PSA_KEY_TYPE_AES`, bits must be 128, algorithm must include
   `PSA_ALG_CCM`, usage must include `ENCRYPT | DECRYPT`.
2. Derive subkeys via HKDF: `K_enc` for AES-CCM, `K_essiv` for IV
   derivation.
3. Store derived key IDs in module state.

All public API functions validate non-NULL pointer parameters at entry.
In debug builds (`CONFIG_ASSERT=y`), violations trigger `__ASSERT`.
In release builds, functions return `-EINVAL`.

```c
/* Per-type header encryption / decryption.
 * AAD is constructed internally from the header struct and context
 * to prevent caller misuse. */
int ubi_crypto_seal_ec_hdr(struct ubi_ec_hdr *hdr,
                           uint32_t peb_idx);
int ubi_crypto_open_ec_hdr(struct ubi_ec_hdr *hdr,
                           uint32_t peb_idx);

int ubi_crypto_seal_vid_hdr(struct ubi_vid_hdr *hdr,
                            uint32_t peb_idx);
int ubi_crypto_open_vid_hdr(struct ubi_vid_hdr *hdr,
                            uint32_t peb_idx);

int ubi_crypto_seal_dev_hdr(struct ubi_dev_hdr *hdr);
int ubi_crypto_open_dev_hdr(struct ubi_dev_hdr *hdr);

int ubi_crypto_seal_vol_hdr(struct ubi_vol_hdr *hdr,
                            uint8_t vol_index);
int ubi_crypto_open_vol_hdr(struct ubi_vol_hdr *hdr,
                            uint8_t vol_index);

/* LEB data encryption / decryption */
int ubi_crypto_seal_data(const struct ubi_crypto_aad_info *info,
                         const uint8_t *plaintext, size_t len,
                         uint8_t *iv_out,
                         uint8_t *ciphertext,
                         uint8_t *tag_out);

int ubi_crypto_open_data(const struct ubi_crypto_aad_info *info,
                         const uint8_t *ciphertext, size_t len,
                         const uint8_t *iv,
                         const uint8_t *tag,
                         uint8_t *plaintext);

/* ESSIV nonce derivation */
int ubi_crypto_derive_essiv(const uint8_t *input, size_t input_len,
                            uint8_t *iv_out);

/* Sequence number notification */
/** Force an immediate sqnum notification callback invocation.
 *  Call before sleep, after critical writes, or at application checkpoints.
 *  Without this, the application may miss up to sqnum_notify_delta
 *  writes since the last notification. */
int ubi_crypto_notify_sqnum(struct ubi_device *ubi);
```

### 9.4 PSA Error Code Mapping

All functions return POSIX error codes. PSA errors are mapped as:

| PSA Status                    | Return    | Meaning                    |
|-------------------------------|-----------|----------------------------|
| `PSA_SUCCESS`                 | `0`       | OK                         |
| `PSA_ERROR_INVALID_SIGNATURE` | `-EBADMSG`| Authentication failure     |
| `PSA_ERROR_INVALID_ARGUMENT`  | `-EINVAL` | Bad input parameters       |
| `PSA_ERROR_NOT_PERMITTED`     | `-EACCES` | Key usage violation        |
| Other PSA errors              | `-EIO`    | Generic crypto failure     |

### 9.5 Header Type Enum (internal)

Used internally by the crypto module for dispatch and AAD construction.
Not exposed in the per-type public API.

```c
enum ubi_hdr_type {
    UBI_HDR_TYPE_DEV,
    UBI_HDR_TYPE_VOL,
    UBI_HDR_TYPE_EC,
    UBI_HDR_TYPE_VID,
};
```

---

## 10. External AAD Callback

### 10.1 AAD Info Structure

For LEB data encryption, UBI provides flash context to the callback:

```c
struct ubi_crypto_aad_info {
    uint32_t peb_idx;
    uint32_t ec;
    uint32_t leb_num;
    uint32_t vol_id;
    uint64_t sqnum;
    uint32_t data_size;
};
```

### 10.2 Callback Typedef

```c
/**
 * Provide additional AAD for LEB data encryption.
 *
 * UBI calls this before encrypting/decrypting LEB data. The callback
 * appends application-specific AAD (e.g. firmware version, device ID)
 * to the UBI-provided flash context.
 *
 * @param info   Flash context for the current operation.
 * @param buf    Output buffer for additional AAD bytes.
 * @param len    In: buffer capacity. Out: bytes written.
 * @return 0 on success, negative error code on failure.
 */
typedef int (*ubi_crypto_aad_cb)(const struct ubi_crypto_aad_info *info,
                                 uint8_t *buf, size_t *len);
```

### 10.3 Usage

When `CONFIG_UBI_CRYPTO_EXTERNAL_AAD` is enabled and `aad_cb` is not
NULL, UBI:
1. Builds internal AAD from `ubi_crypto_aad_info` fields.
2. Calls `aad_cb()` to get additional bytes.
3. Concatenates internal + external AAD.
4. Passes combined AAD to `psa_aead_encrypt()` / `psa_aead_decrypt()`.

---

## 11. Integration Points

The crypto layer intercepts 26 existing I/O operations across 5 source
files. All interceptions are wrapped in `#if defined(CONFIG_UBI_CRYPTO)`.

### 11.1 Header Reads (decrypt after flash_area_read)

| File             | Function                  | Header Type |
|------------------|---------------------------|-------------|
| `ubi_io.c`       | `ubi_ec_hdr_read()`       | EC          |
| `ubi_io.c`       | `ubi_vid_hdr_read()`      | VID         |
| `ubi_io.c`       | `ubi_vol_hdr_read()`      | Volume      |
| `ubi_res_peb.c`  | `ubi_res_peb_scan()`      | Device      |
| `ubi_res_peb.c`  | `ubi_res_peb_scan()`      | Volume      |
| `ubi_res_peb.c`  | `ubi_res_peb_read_content()` | Device+Vol |

### 11.2 Header Writes (encrypt before flash_area_write)

| File             | Function                      | Header Type |
|------------------|-------------------------------|-------------|
| `ubi_io.c`       | `ubi_ec_hdr_write()`          | EC          |
| `ubi_io.c`       | `ubi_vid_hdr_write()`         | VID         |
| `ubi_io.c`       | `ubi_dev_mount()`             | Device      |
| `ubi_res_peb.c`  | `res_peb_recover()`           | Device+Vol  |
| `ubi_res_peb.c`  | `ubi_res_peb_overwrite()` (3x)| Device+Vol  |

### 11.3 LEB Data I/O (encrypt/decrypt payload)

| File             | Function                  | Operation |
|------------------|---------------------------|-----------|
| `ubi_io.c`       | `ubi_leb_data_write()`    | Encrypt   |
| `ubi_io.c`       | `ubi_leb_data_read()`     | Decrypt   |

### 11.4 CRC Computations (14 total)

CRC remains over plaintext. Computation order changes:
- **Write**: compute CRC → encrypt → write
- **Read**: read → decrypt → verify CRC

No changes to CRC call sites themselves — they operate on the
plaintext struct before/after crypto.

---

## 12. Kconfig Options

```kconfig
config UBI_CRYPTO
    bool "Enable UBI crypto layer"
    depends on UBI
    depends on MBEDTLS
    depends on MBEDTLS_PSA_CRYPTO_C
    help
      Encrypt and authenticate all UBI on-flash structures using
      AES-128-CCM via the PSA Crypto API. Increases header sizes
      and reduces usable LEB size by 80 bytes.

config UBI_CRYPTO_SQNUM_NOTIFY
    bool "Sequence number notification callback"
    depends on UBI_CRYPTO
    help
      Enable a configurable callback that UBI invokes every N LEB
      writes with the current global sequence number. The application
      can use this to persist the sqnum to secure storage and
      implement anti-rollback detection externally.

config UBI_CRYPTO_EXTERNAL_AAD
    bool "External AAD callback for LEB data"
    depends on UBI_CRYPTO
    help
      Allow applications to provide additional authenticated data
      for LEB payload encryption via a callback. Useful for
      binding data to application-specific context.

config UBI_CRYPTO_KEY_ROTATION
    bool "Key rotation support"
    depends on UBI_CRYPTO
    help
      Enable ubi_crypto_rekey() for online re-encryption with a
      new symmetric key. Adds rotation state tracking to the
      device header.
```

---

## 13. Testing Strategy

### 13.1 New Test Suite: `ubi_crypto` (~16 tests)

**Unit tests:**

| # | Test | Description |
|---|------|-------------|
| 1 | `seal_open_roundtrip_ec_hdr` | Encrypt then decrypt EC header, verify fields match |
| 2 | `seal_open_roundtrip_vid_hdr` | Same for VID header |
| 3 | `seal_open_roundtrip_dev_hdr` | Same for device header |
| 4 | `seal_open_roundtrip_vol_hdr` | Same for volume header |
| 5 | `seal_open_roundtrip_leb_data` | Encrypt/decrypt LEB payload with AAD |
| 6 | `tampered_tag_rejected` | Flip bit in auth tag, verify decrypt fails |
| 7 | `tampered_ciphertext_rejected` | Flip bit in ciphertext, verify decrypt fails |
| 8 | `essiv_deterministic` | Same inputs produce same IV |
| 9 | `essiv_location_binding` | Different peb_idx produces different IV |
| 10 | `wrong_key_rejected` | Decrypt with different key fails |

**Integration tests:**

| #  | Test | Description |
|----|------|-------------|
| 11 | `crypto_init_deinit_reboot` | Init, write, deinit, reinit, read back |
| 12 | `crypto_volume_lifecycle` | Create volume, write, reboot, read — all encrypted |
| 13 | `sqnum_notify_called` | Verify sqnum_notify_cb called after delta writes |
| 14 | `sqnum_notify_deinit` | Verify sqnum_notify_cb called during deinit |
| 15 | `external_aad_binding` | Write with AAD, read with different AAD, expect failure |
| 16 | `peb_relocation_detected` | Copy encrypted PEB to different offset, verify ESSIV rejection |

**Key rotation tests (if CONFIG_UBI_CRYPTO_KEY_ROTATION):**

| #  | Test | Description |
|----|------|-------------|
| 17 | `rekey_roundtrip` | Write data, rekey, read back with new key |
| 18 | `rekey_crash_resume` | Simulate power loss mid-rotation, verify resume on init |

### 13.2 Test Infrastructure

- Mock PSA key: 16-byte AES key created in test `before()` fixture.
- Mock `sqnum_notify_cb`: records last-notified sqnum in static variable.
- Mock `aad_cb`: returns fixed application context bytes.
- Existing 101 tests pass unchanged with `CONFIG_UBI_CRYPTO=n`.
- Existing tests also pass with `CONFIG_UBI_CRYPTO=y` (functional compatibility).

---

## 14. Phased Implementation Roadmap

| Phase | Scope | Depends On | Tests (cumulative) |
|-------|-------|------------|--------------------|
| 1 | Header struct changes + conditional size macros | — | 0 (compile-only) |
| 2 | `ubi_crypto.h/.c` — seal/open/ESSIV functions | Phase 1 | 10 (unit: 1–10) |
| 3 | I/O layer integration — encrypt/decrypt in read/write paths | Phase 2 | 13 (+3: 11, 12, 16) |
| 4 | Sqnum notification — lazy callback, delta config | Phase 3 | 15 (+2: 13, 14) |
| 5 | External AAD callback | Phase 3 | 16 (+1: 15) |
| 6 | Key rotation — rekey API, crash resume | Phase 3 | 18 (+2: 17, 18) |

Each phase is a self-contained commit that compiles and passes all tests.

---

## 15. Open Questions

| # | Question | Options | Recommendation |
|---|----------|---------|----------------|
| Q1 | Header version bump with crypto? | (a) Bump `_VERSION` to 2, (b) Add separate `crypto_version` field | (a) Simpler; scan can distinguish crypto vs. plaintext without trial decryption |
| Q2 | `ubi_crypto_rekey()` blocking or background? | (a) Synchronous (hold mutex), (b) Background with per-PEB locking | (a) for v0.11; background as future enhancement |
| Q3 | LEB data: single AES-CCM or chunked? | (a) Single over full payload, (b) Per-chunk with chaining | (a) Simpler; caller already provides full buffer |
| Q4 | ~~Rollback detection action?~~ | Resolved | UBI exposes sqnum via `ubi_device_get_info()` + lazy notification callback; application implements its own rollback policy (see §7) |
| Q5 | Random IV for dev/vol hdrs acceptable? | Both banks get different ciphertext for same plaintext | Yes — banks are independent mirrors; different IV per bank is correct |
| Q6 | What if power lost after old key destroyed mid-rotation? | Data loss | UBI must document: never destroy old key before `rekey()` returns. UBI itself never destroys keys |
| Q7 | Algorithm selection? | (a) Hardcode AES-128-CCM, (b) `CONFIG_UBI_CRYPTO_ALGORITHM` Kconfig | (a) for v0.11; single algorithm reduces complexity and test surface |

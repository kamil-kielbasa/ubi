# UBI Secure On-Flash Architecture

**Status:** final architecture  
**Scope:** secure UBI device format for Zephyr  
**Audience:** UBI developers and maintainers

---

## 1. Two-minute overview

A UBI device works in exactly one mode:

- **PLAIN**
- **SECURE**

In **SECURE** mode, UBI encrypts and authenticates five object classes:

- **device header**
- **volume header**
- **erase-counter header**
- **volume-identifier header**
- **LEB payload**

The encrypted payload of each object is still the existing plain UBI structure from the codebase:

- `struct ubi_dev_hdr` = 32 B
- `struct ubi_vol_hdr` = 48 B
- `struct ubi_ec_hdr` = 16 B
- `struct ubi_vid_hdr` = 32 B

That keeps the **meaning** of the current on-flash UBI headers unchanged, while adding a secure wrapper around them. The existing project also already enforces `WRITE_BLOCK_SIZE_ALIGNMENT == 16`, and all plain UBI headers are multiples of 16 bytes, which this design keeps compatible with.

The secure wrapper is built around **AES-CCM** with a fixed 13-byte nonce and a 16-byte authentication tag.

```text
nonce = domain(1B) || salt(6B) || counter(6B)
```

Each secure object starts with a **16-byte common prefix**:

```text
+--------+--------+-------------+----------------+
| magic  | domain | key_version | salt || ctr    |
| 2B     | 1B     | 1B          | 6B   || 6B     |
+--------+--------+-------------+----------------+
```

The prefix is plaintext, but the whole prefix is authenticated because it is always included in **AAD**.

The design uses one versioned root key material input `IKM[v]` and derives child keys for:

- `K_dev[v]`
- `K_vol[v]`
- `K_ec[v]`
- `K_vid[v]`
- `K_leb[v][vol_id]`

This gives:

- **domain separation**,
- **location-binding** through AAD fields derived from physical flash location,
- **data-binding** through AAD fields derived from already-authenticated parent objects,
- simple nonce construction,
- restart recovery by scanning prefixes already stored on flash.

### 1.1 High-level picture

```text
+------------------------------+       +---------------------------------+
| Application / secure storage |       | Internal trusted monotonic data |
| - versioned IKM[v]           |       | - app-specific rollback state   |
| - key provisioning           |       | - optional secure counter store |
+---------------+--------------+       +----------------+----------------+
                |                                       |
                | get_ikm()                             | sqnum_init_check()
                |                                       | sqnum_sync()
                v                                       v
+--------------------------------------------------------------------------+
| UBI core                                                                  |
| - volume table                                                            |
| - PEB allocator                                                           |
| - global_sqnum                                                            |
| - secure usage cache per {domain, key_version, vol_id?}                  |
+----------------------------------+---------------------------------------+
                                   |
                                   | AES-CCM wrapper
                                   v
+--------------------------------------------------------------------------+
| Secure on-flash objects                                                   |
|                                                                          |
| Reserved PEB area:                                                        |
|   [device header wrapper] ---> binds ---> [volume header wrappers]       |
|                                                                          |
| Data PEB:                                                                 |
|   [EC header wrapper] ---> [VID header wrapper] ---> [LEB payload]       |
+--------------------------------------------------------------------------+
```

---

## 2. Cryptographic profile

UBI SECURE uses **AES-CCM** because it fits the object-based storage model well:

- fixed-size metadata records,
- explicit AAD,
- explicit nonce per object,
- hardware acceleration on the current target families,
- no need for stream-style processing.

The cryptographic profile is:

- algorithm: **AES-CCM**,
- nonce length: **13 bytes**,
- tag length: **16 bytes**,
- key size: **one fixed AES key size per secure device configuration**.

### 2.1 AES key size

Architecturally, AES supports 128-, 192-, and 256-bit keys.

For this design, one secure device uses **one configured AES key size only**. Mixed key sizes inside one secure UBI device are not supported.

For the current hardware-accelerated target set:

- nRF5340 CryptoCell-312 supports AES-CCM and AES-GCM, and its AES-CCM path includes 192- and 256-bit key support on CryptoCell-312.
- STM32U585 AES supports CTR, CCM, GCM, and GMAC, and its datasheet documents 128- and 256-bit cipher-key support for that accelerator.

For a common cross-target hardware-accelerated baseline, **128 or 256 bits** are therefore the portable choices across nRF5340 and STM32U585.

### 2.2 Why 13-byte nonce

CCM requires:

```text
n + q = 15
```

With:

- `n = 13` bytes of nonce,
- `q = 2` bytes of encoded payload length,

one AEAD operation can carry:

```text
payload_len < 2^(8*q) = 65536 bytes
```

That is comfortably above current 4 KiB and 8 KiB UBI payload sizes.

Changing the AES key size does **not** change the nonce format, nonce length, tag length, or the `n + q = 15` relation. Those are properties of the chosen CCM profile, not of the AES key size itself.

---

## 3. Key hierarchy

### 3.1 Root key material

The application provides a versioned input key material value:

```text
IKM[v]
```

where `v` is the key version.

UBI derives child keys from `IKM[v]` using HKDF-SHA-256.

### 3.2 Derived keys

For one key version `v`:

```text
K_dev[v]          = HKDF(IKM[v], "UBI|DEV")
K_vol[v]          = HKDF(IKM[v], "UBI|VOL")
K_ec[v]           = HKDF(IKM[v], "UBI|EC")
K_vid[v]          = HKDF(IKM[v], "UBI|VID")
K_leb[v][vol_id]  = HKDF(IKM[v], "UBI|LEB|vol=%u", vol_id)
```

### 3.3 What each key protects

- `K_dev[v]` protects the **device header**.
- `K_vol[v]` protects all **volume headers**.
- `K_ec[v]` protects all **erase-counter headers**.
- `K_vid[v]` protects all **volume-identifier headers**.
- `K_leb[v][vol_id]` protects only **LEB payloads belonging to one volume**.

### 3.4 Binding chain

The hierarchy is not only about key separation. It also defines a dependency chain for decryption and validation:

```text
device header
   └── volume header

erase-counter header
   └── volume-identifier header
          └── LEB payload
```

This creates both:

- **location-binding** — the object is tied to a specific place on flash,
- **data-binding** — the child object is tied to authenticated parent metadata.

In practice, an attacker who can only decrypt or tamper with one domain does not automatically learn the hidden parent fields required to authenticate child-domain AAD.

---

## 4. Common prefix and nonce

### 4.1 Common prefix

Every secure object begins with the same 16-byte prefix:

```c
struct ubi_crypto_prefix16 {
    uint16_t magic;
    uint8_t  domain;
    uint8_t  key_version;
    uint8_t  salt[6];
    uint8_t  counter[6];
};
_Static_assert(sizeof(struct ubi_crypto_prefix16) == 16,
               "ubi_crypto_prefix16 must be 16 bytes");
```

### 4.2 Prefix rules

- `magic` is at the **beginning** of the prefix.
- The prefix does **not** contain a trailing CRC32.
- Prefix integrity is provided by the **AEAD tag**, because the full prefix is always part of AAD.
- The existing `hdr_crc` fields remain inside the encrypted plain UBI headers and keep their current semantic role.

### 4.3 Domain values

The `domain` byte identifies the object class:

- `UBI_CRYPTO_DOMAIN_DEV`
- `UBI_CRYPTO_DOMAIN_VOL`
- `UBI_CRYPTO_DOMAIN_EC`
- `UBI_CRYPTO_DOMAIN_VID`
- `UBI_CRYPTO_DOMAIN_LEB`

### 4.4 Nonce construction

The nonce is reconstructed directly from the prefix:

```text
nonce[0]      = domain
nonce[1..6]   = salt[6]
nonce[7..12]  = counter[6]
```

So the full nonce is:

```text
nonce = domain(1B) || salt(6B) || counter(6B)
```

### 4.5 Salt generation

`salt[6]` is generated freshly for each write from a cryptographically strong RNG / TRNG.

It is intentionally part of the nonce so that the nonce remains unique with very high probability even if the newest prefix is lost before reboot and `next_counter` is later reconstructed from older flash-visible state.

### 4.6 Counter uniqueness rule

Nonce uniqueness is guaranteed by the tuple:

```text
(key, domain, salt, counter)
```

Two secure objects are safe with respect to nonce reuse as long as the same tuple above is not repeated.

---

## 5. Secure object layout

### 5.1 Existing plain payloads stay unchanged

The encrypted inner payloads are the current plain UBI structures:

```text
device header            -> struct ubi_dev_hdr (32 B)
volume header            -> struct ubi_vol_hdr (48 B)
erase-counter header     -> struct ubi_ec_hdr  (16 B)
volume-identifier header -> struct ubi_vid_hdr (32 B)
LEB payload              -> raw data bytes
```

This keeps the semantic payload unchanged and minimizes drift between the existing PLAIN implementation and the SECURE implementation. The current repo defines these sizes and also enforces 16-byte alignment for them.

### 5.2 Secure wrappers

#### Device header wrapper

```text
+----------+--------------------------------+--------+
| prefix16 | ciphertext(device_header, 32B) | tag16  |
+----------+--------------------------------+--------+
= 64 bytes
```

#### Volume header wrapper

```text
+----------+--------------------------------+--------+
| prefix16 | ciphertext(volume_header, 48B) | tag16  |
+----------+--------------------------------+--------+
= 80 bytes
```

#### Erase-counter header wrapper

```text
+----------+---------------------------------------+--------+
| prefix16 | ciphertext(erase_counter_header, 16B) | tag16  |
+----------+---------------------------------------+--------+
= 48 bytes
```

#### Volume-identifier header wrapper

```text
+----------+-------------------------------------------+--------+
| prefix16 | ciphertext(volume_identifier_header, 32B) | tag16  |
+----------+-------------------------------------------+--------+
= 64 bytes
```

#### LEB payload wrapper

LEB payload uses a longer prefix:

```c
struct ubi_leb_prefix32 {
    struct ubi_crypto_prefix16 common;
    uint64_t bytes_after_write;
    uint8_t  reserved[8];
};
_Static_assert(sizeof(struct ubi_leb_prefix32) == 32,
               "ubi_leb_prefix32 must be 32 bytes");
```

On flash:

```text
+--------------+-------------------------+--------+
| leb_prefix32 | ciphertext(LEB payload) | tag16  |
+--------------+-------------------------+--------+
```

`bytes_after_write` stores the cumulative number of plaintext payload bytes written with the current `{key_version, vol_id}` LEB key scope up to and including this write.

`reserved[8]` remains available for future use.

### 5.3 No packed structs

The secure on-flash structures do **not** use `packed`.

The implementation shall:

- rely on fixed-width fields and arrays,
- verify sizes with `_Static_assert(sizeof(...))`,
- keep every fixed wrapper size a multiple of 16 bytes.

---

## 6. AAD, location-binding, and data-binding

### 6.1 General rule

AAD is built from two sources:

1. **plaintext control data**, available before decrypt,
2. **already-authenticated hidden metadata** from parent objects.

That creates two distinct bindings:

- **location-binding**: the object is tied to a specific physical place on flash,
- **data-binding**: the object is tied to authenticated parent metadata.

### 6.2 Visual overview

```text
Reserved area:

  prefix16 + location
        |
        v
  device header AAD
        |
        +----> decrypted device header fields
                     |
                     v
               volume header AAD


Data area:

  prefix16 + location
        |
        v
  erase-counter header AAD
        |
        +----> decrypted erase-counter header fields
                     |
                     v
               volume-identifier header AAD
                             |
                             +----> decrypted volume-identifier header fields
                                          |
                                          v
                                    LEB payload AAD
```

### 6.3 AAD fields by object

#### Device header AAD

Source fields:

- full `prefix16` from flash,
- reserved-PEB physical index from runtime geometry,
- device-header byte offset in the reserved area from runtime geometry.

#### Volume header AAD

Source fields:

- full `prefix16` from flash,
- reserved-PEB physical index from runtime geometry,
- volume-header byte offset in the reserved area from runtime geometry,
- `device_header.revision` from the already-authenticated decrypted device header,
- `device_header.version` from the already-authenticated decrypted device header,
- `device_header.hdr_crc` from the already-authenticated decrypted device header.

#### Erase-counter header AAD

Source fields:

- full `prefix16` from flash,
- physical PEB index from runtime geometry,
- erase-counter-header byte offset from runtime geometry.

#### Volume-identifier header AAD

Source fields:

- full `prefix16` from flash,
- physical PEB index from runtime geometry,
- volume-identifier-header byte offset from runtime geometry,
- `erase_counter_header.ec` from the already-authenticated decrypted erase-counter header,
- `erase_counter_header.hdr_crc` from the already-authenticated decrypted erase-counter header.

#### LEB payload AAD

Source fields:

- full `leb_prefix32` from flash,
- physical PEB index from runtime geometry,
- data byte offset from runtime geometry,
- `erase_counter_header.ec` from the already-authenticated decrypted erase-counter header,
- `erase_counter_header.hdr_crc` from the already-authenticated decrypted erase-counter header,
- `volume_identifier_header.vol_id` from the already-authenticated decrypted volume-identifier header,
- `volume_identifier_header.lnum` from the already-authenticated decrypted volume-identifier header,
- `volume_identifier_header.sqnum` from the already-authenticated decrypted volume-identifier header,
- `volume_identifier_header.data_size` from the already-authenticated decrypted volume-identifier header,
- `volume_identifier_header.hdr_crc` from the already-authenticated decrypted volume-identifier header.

### 6.4 Fixed AAD lengths used by this design

With the field sets above, the design uses the following AAD lengths:

| Object | AAD length |
|---|---:|
| device header | 24 B |
| volume header | 33 B |
| erase-counter header | 24 B |
| volume-identifier header | 32 B |
| LEB payload | 68 B |

These lengths matter for estimating total AES block-cipher invocations per key scope.

---

## 7. Counter scopes and mount recovery

### 7.1 Counter scope keys

A counter is not global for the whole device. It belongs to a **scope key**.

The scope key is:

- **device header**: `{domain=DEV, key_version}`
- **volume header**: `{domain=VOL, key_version}`
- **erase-counter header**: `{domain=EC, key_version}`
- **volume-identifier header**: `{domain=VID, key_version}`
- **LEB payload**: `{domain=LEB, key_version, vol_id}`

That means different key versions are tracked independently, and LEB usage is tracked independently for each volume.

### 7.2 First format

On the first secure format:

- the first object in each scope uses counter `0`,
- each later object in the same scope increments that scope by `1`.

Example:

If the format writes 2048 erase-counter headers, then after format:

```text
next_counter{EC, key_version=v} = 2048
```

### 7.3 Mount recovery

On every secure mount, UBI scans all secure prefixes and rebuilds the in-RAM usage cache.

For each scope key, UBI computes:

```text
next_counter(scope) = max(counter_seen_for_scope) + 1
```

For LEB payloads, UBI also computes:

```text
next_bytes(scope) = max(bytes_after_write_seen_for_scope)
```

### 7.4 Multiple key versions on flash

If secure objects written under several key versions exist on flash at the same time, UBI keeps **separate** usage state for each scope key.

Example:

```text
{domain=VID, key_version=1}
{domain=VID, key_version=2}
{domain=LEB, key_version=2, vol_id=7}
{domain=LEB, key_version=3, vol_id=7}
```

All four states are independent.

### 7.5 Why key_version belongs to the usage cache

`key_version` must be part of the in-RAM usage cache key because older secure objects may still exist on flash and must still be readable after the write key version is advanced.

The config only tells UBI which version to use for **new writes**. The flash may still contain older versions, and their usage state must remain distinct.

---

## 8. Key-usage limits

## 8.1 Simple view

For day-to-day engineering, the rule is simple:

- every secure write consumes **one** counter value from its scope key,
- the counter field is **48 bits**,
- so one scope key can consume at most:

```text
2^48 counter values
```

before wrap.

For the supported UBI metadata sizes and 4/8 KiB LEB payload sizes, this 48-bit counter bound is the practical lifetime limit.

### Rotation policy

UBI should emit rotation warnings based on **counter usage**:

- **ROTATE_SOON** when `next_counter >= 2^47`
- **ROTATE_NOW** when the next write would require `next_counter >= 2^48`

For LEB scopes, UBI also exposes `bytes_after_write` as an extra operational metric.

## 8.2 Deeper view: AES block-cipher invocations

NIST SP 800-38C also requires that total block-cipher usage under one CCM key stays bounded. For one write, a practical engineering upper bound is:

```text
payload_blocks = ceil(payload_len / 16)
aad_blocks     = ceil((2 + aad_len) / 16)
aes_calls      = 2 + aad_blocks + 2 * payload_blocks
```

Where:

- the leading `2` accounts for the initial `B0` processing and `Ctr0`,
- `aad_blocks` covers formatted AAD,
- `2 * payload_blocks` covers CBC-MAC over payload plus CTR encryption of payload.

### 8.3 Per-domain AES-call estimates

Using the fixed payload sizes from `ubi_io.h` and the AAD sets defined in this document:

| Scope key | Payload length | AAD length | AES calls per write |
|---|---:|---:|---:|
| `{DEV, v}` | 32 B | 24 B | `2 + 2 + 2*2 = 8` |
| `{VOL, v}` | 48 B | 33 B | `2 + 3 + 2*3 = 11` |
| `{EC, v}`  | 16 B | 24 B | `2 + 2 + 2*1 = 6` |
| `{VID, v}` | 32 B | 32 B | `2 + 3 + 2*2 = 9` |
| `{LEB, v, vol_id}` | `N` B | 68 B | `2 + 5 + 2*ceil(N/16)` |

For `N = 4096`:

```text
aes_calls_leb = 7 + 2*256 = 519
```

For `N = 8192`:

```text
aes_calls_leb = 7 + 2*512 = 1031
```

### 8.4 Why the 48-bit counter is still enough

Even in the hottest current path:

```text
2^48 writes * 1031 AES calls/write < 2^61
```

So for the currently targeted 4 KiB and 8 KiB UBI payload sizes, the 48-bit counter remains the tighter practical limit.

### 8.5 What UBI tracks

UBI therefore tracks and reports, per scope key:

- `next_counter`
- `usage_pct = floor(100 * next_counter / 2^48)`
- for LEB only: `bytes_after_write`
- a derived `est_aes_calls = next_counter * calls_per_write` for fixed-size scopes,
- a derived `est_aes_calls_max = next_counter * (7 + 2*ceil(leb_size/16))` for LEB scopes.

The **hard stop** remains counter exhaustion. The AES-call estimate is included for visibility and standards-driven review.

---

## 9. Secure write path

### 9.1 Common steps

For every secure write:

1. choose the active `write_key_version`,
2. obtain `IKM[write_key_version]`,
3. derive the child key for the target domain,
4. load and increment the in-RAM counter for the scope key,
5. generate fresh `salt[6]`,
6. build `prefix16` or `leb_prefix32`,
7. build AAD,
8. run AES-CCM encrypt,
9. write prefix + ciphertext + tag to flash.

### 9.2 Device header write

- key: `K_dev[v]`
- scope key: `{DEV, v}`
- wrapper: `prefix16 + ciphertext(device header) + tag16`

### 9.3 Volume header write

- key: `K_vol[v]`
- scope key: `{VOL, v}`
- wrapper: `prefix16 + ciphertext(volume header) + tag16`

### 9.4 Erase-counter header write

- key: `K_ec[v]`
- scope key: `{EC, v}`
- wrapper: `prefix16 + ciphertext(erase-counter header) + tag16`

### 9.5 Volume-identifier header write

- key: `K_vid[v]`
- scope key: `{VID, v}`
- wrapper: `prefix16 + ciphertext(volume-identifier header) + tag16`

### 9.6 LEB payload write

- key: `K_leb[v][vol_id]`
- scope key: `{LEB, v, vol_id}`
- wrapper: `leb_prefix32 + ciphertext(LEB payload) + tag16`

For LEB:

```text
bytes_after_write = previous_bytes_after_write + data_len
```

and that value is stored in `leb_prefix32`.

### 9.7 Publication order inside a data PEB

For a new logical block version, the publication order is:

1. prepare the target PEB,
2. write the erase-counter header,
3. write the encrypted LEB payload,
4. write the encrypted volume-identifier header as the commit-visible metadata.

This keeps the volume-identifier header as the final metadata step that publishes the new mapping.

---

## 10. Secure read path

For every secure read:

1. read the prefix,
2. check `magic`,
3. select the key by `{domain, key_version}` and, for LEB, later by `vol_id`,
4. rebuild the 13-byte nonce from the prefix,
5. rebuild AAD from the prefix, flash location, and already-authenticated parent metadata,
6. run AES-CCM decrypt+verify,
7. only after successful verification, expose the inner plain payload.

### 10.1 Read-order dependency for LEB

To read an LEB payload:

1. authenticate and decrypt the erase-counter header,
2. authenticate and decrypt the volume-identifier header,
3. derive `K_leb[v][vol_id]` using the authenticated `vol_id`,
4. authenticate and decrypt the LEB payload.

This is the intended data-binding chain.

---

## 11. Rollback detection substrate

UBI does **not** claim to solve anti-rollback by itself.

UBI provides the substrate for the application to do that safely:

- UBI reconstructs and stores `global_sqnum`,
- the application can query it explicitly,
- UBI can notify the application at mount time,
- UBI can notify the application later every time `global_sqnum` advances by a configured delta.

### 11.1 Mount-time callback

After a successful secure mount and after `global_sqnum` is known, UBI calls a callback so the application can compare UBI's value against its own trusted value stored elsewhere.

The application decides whether rollback is suspected and may abort init by returning an error.

### 11.2 Runtime sync callback

UBI also emits a delta-based callback whenever `global_sqnum` advanced enough that the application should persist or compare it with its own trusted storage.

---

## 12. Edge cases

### 12.1 Lost newest object before reboot

If the newest secure object is lost before reboot, mount recovery may rebuild `next_counter` from an older prefix.

That is why the nonce also contains a fresh 48-bit `salt`.

The counter provides operational monotonicity, and `salt` keeps accidental nonce reuse highly improbable even if the newest persisted counter state disappears.

### 12.2 Average erase counter recovery

If UBI needs to assign a new erase-counter value using an `ec_avg` heuristic for a reused PEB, that does not affect nonce uniqueness.

The erase-counter-header nonce is based on:

```text
K_ec[v] + domain + salt + counter
```

not on the erase-counter value itself.

So `ec_avg` and nonce uniqueness are decoupled.

### 12.3 Damaged parent metadata

If a parent object fails authentication:

- its hidden fields are not trusted,
- child AAD that depends on those fields cannot be built,
- the child object is therefore not considered readable.

This is intentional.

---

## 13. API proposal

There is no separate `ubi_device_init_ex()`. The public init API itself carries the secure configuration.

### 13.1 Configuration structures

```c
/**
 * @brief Compare the global UBI sequence number recovered during mount with
 *        the application's own trusted sequence number.
 *
 * This callback is invoked once after mount, before ubi_device_init() returns
 * success. The application may read its own trusted rollback state and decide
 * whether the recovered UBI value is acceptable.
 *
 * @param ubi_sqnum  Global sequence number reconstructed by UBI.
 * @param user_ctx   User pointer from struct ubi_crypto_cfg.
 *
 * @retval 0         Accept mount.
 * @retval <0        Reject mount and fail initialization.
 */
typedef int (*ubi_sqnum_init_check_cb)(uint64_t ubi_sqnum, void *user_ctx);

/**
 * @brief Persist or compare the global UBI sequence number during runtime.
 *
 * This callback is invoked whenever global_sqnum advanced by cfg->sqnum_sync_delta.
 *
 * @param ubi_sqnum  Current global sequence number.
 * @param user_ctx   User pointer from struct ubi_crypto_cfg.
 *
 * @retval 0         Success.
 * @retval <0        Error is reported through a security event.
 */
typedef int (*ubi_sqnum_sync_cb)(uint64_t ubi_sqnum, void *user_ctx);

/**
 * @brief Return the versioned root key material for one key version.
 *
 * UBI calls this on demand for the active write key version and for any older
 * key version encountered while reading secure objects from flash.
 *
 * @param key_version  Requested key version.
 * @param ikm_buf      Output buffer for IKM.
 * @param ikm_len      In: buffer capacity. Out: actual IKM length.
 * @param user_ctx     User pointer from struct ubi_crypto_cfg.
 *
 * @retval 0           Success.
 * @retval <0          Key version unavailable.
 */
typedef int (*ubi_crypto_get_ikm_cb)(uint8_t key_version,
                                     uint8_t *ikm_buf,
                                     size_t *ikm_len,
                                     void *user_ctx);

/** @brief Security event kinds reported by UBI SECURE. */
enum ubi_security_event_type {
    UBI_SEC_EVT_POLICY_MISMATCH,
    UBI_SEC_EVT_AUTH_FAILURE,
    UBI_SEC_EVT_KEY_ROTATE_SOON,
    UBI_SEC_EVT_KEY_ROTATE_NOW,
};

/** @brief Authentication failure details. */
struct ubi_sec_evt_auth_failure {
    uint8_t  domain;
    uint8_t  key_version;
    uint32_t pnum;
    int      rc;
};

/** @brief Key-budget event details. */
struct ubi_sec_evt_key_budget {
    uint8_t  domain;
    uint8_t  key_version;
    uint32_t vol_id;           /* meaningful only for LEB */
    uint64_t next_counter;
    uint64_t bytes_after_write;/* meaningful only for LEB */
    uint8_t  usage_pct;
};

/** @brief Policy mismatch details. */
struct ubi_sec_evt_policy_mismatch {
    bool secure_required;
    bool flash_looks_secure;
};

/** @brief Tagged security event. */
struct ubi_security_event {
    enum ubi_security_event_type type;
    union {
        struct ubi_sec_evt_auth_failure    auth_failure;
        struct ubi_sec_evt_key_budget      key_budget;
        struct ubi_sec_evt_policy_mismatch policy_mismatch;
    } u;
};

/**
 * @brief Report a security-relevant event.
 *
 * @param evt       Event payload.
 * @param user_ctx  User pointer from struct ubi_crypto_cfg.
 */
typedef void (*ubi_security_event_cb)(const struct ubi_security_event *evt,
                                      void *user_ctx);

/** @brief Secure UBI configuration. */
struct ubi_crypto_cfg {
    bool enabled;
    bool secure_required;
    uint8_t write_key_version;
    uint64_t sqnum_sync_delta;
    ubi_crypto_get_ikm_cb get_ikm;
    ubi_sqnum_init_check_cb sqnum_init_check;
    ubi_sqnum_sync_cb sqnum_sync;
    ubi_security_event_cb security_event;
    void *user_ctx;
};

/** @brief UBI device configuration. */
struct ubi_cfg {
    struct ubi_crypto_cfg crypto;
};
```

### 13.2 Public APIs

```c
/**
 * @brief Initialize a UBI device in PLAIN or SECURE mode.
 *
 * The flash content must match the requested policy. If secure_required is set
 * and flash does not contain a valid secure layout, initialization fails.
 */
int ubi_device_init(const struct ubi_mtd *mtd,
                    const struct ubi_cfg *cfg,
                    struct ubi_device **ubi);

/**
 * @brief Change the key version used for future writes.
 *
 * Older key versions remain readable as long as get_ikm() can still return
 * their root key material on demand.
 */
int ubi_device_set_write_key_version(struct ubi_device *ubi,
                                     uint8_t key_version);

/**
 * @brief Return the current global UBI sequence number.
 */
int ubi_device_get_global_sqnum(struct ubi_device *ubi,
                                uint64_t *global_sqnum);
```

### 13.3 Usage-state API

```c
/** @brief One secure usage-cache key. */
struct ubi_crypto_scope_key {
    uint8_t  domain;
    uint8_t  key_version;
    uint32_t vol_id; /* meaningful only for LEB */
};

/** @brief Recovered usage state for one scope key. */
struct ubi_crypto_usage_state {
    uint64_t next_counter;
    uint64_t bytes_after_write; /* meaningful only for LEB */
    uint64_t est_aes_calls;
    uint8_t  usage_pct;
};

/**
 * @brief Query recovered secure usage state.
 */
int ubi_crypto_get_usage_state(struct ubi_device *ubi,
                               const struct ubi_crypto_scope_key *scope,
                               struct ubi_crypto_usage_state *state);
```

### 13.4 Key-usage cache sizing

Because secure objects may exist on flash under several key versions, the implementation should size the usage cache through Kconfig.

Recommended knobs:

```c
CONFIG_UBI_CRYPTO_MAX_USAGE_STATES
CONFIG_UBI_CRYPTO_MAX_OPEN_KEY_VERSIONS
```

A usage state is keyed by:

- `{DEV, key_version}`
- `{VOL, key_version}`
- `{EC, key_version}`
- `{VID, key_version}`
- `{LEB, key_version, vol_id}`

### 13.5 Rotation workflow

When UBI emits `UBI_SEC_EVT_KEY_ROTATE_SOON` or `UBI_SEC_EVT_KEY_ROTATE_NOW`, the application performs the key rollout itself:

1. provision `IKM[new_version]` into its own secure storage,
2. keep older versions available for reads as needed,
3. call `ubi_device_set_write_key_version(ubi, new_version)`.

UBI does not fetch a new write key through the event callback itself. The event is a notification, not a provisioning channel.

---

## 14. Device mode and policy

A device is either:

- **PLAIN**
- **SECURE**

There is no mixed mode.

The source of truth is the **init-time policy** provided by the caller.

The on-flash prefix magic is used only to recognize the secure wrapper format during parsing. It is not the authority that decides whether secure mode is acceptable.

If `secure_required == true` and the flash content does not match a valid secure layout, UBI:

- reports `UBI_SEC_EVT_POLICY_MISMATCH`,
- fails initialization.

---

## 15. Glossary

- **AAD** — Additional Authenticated Data. Bytes authenticated by AEAD but not encrypted.
- **AEAD** — Authenticated Encryption with Associated Data.
- **ciphertext** — Encrypted bytes produced by AES-CCM.
- **domain** — One-byte object-class selector: device header, volume header, erase-counter header, volume-identifier header, or LEB payload.
- **key version** — Version selector for `IKM[v]` and all keys derived from it.
- **location-binding** — Binding an object to a physical flash location through AAD.
- **data-binding** — Binding a child object to already-authenticated parent metadata through AAD.
- **scope key** — The tuple that owns a counter and usage state.
- **secure wrapper** — `prefix + ciphertext + tag` format stored on flash.

---

## 16. References

- NIST SP 800-38C — Recommendation for Block Cipher Modes of Operation: The CCM Mode for Authentication and Confidentiality
- RFC 3610 — Counter with CBC-MAC (CCM)
- `lib/src/ubi_io.h` in this repository for current plain-header sizes and 16-byte alignment
- Nordic nRF Connect SDK documentation for `nrf_cc3xx_mbedcrypto`
- STM32U585 datasheet, AES / SAES section

---
orphan: true
---

# Secure Runtime Policy

```{note}
**Legacy page.** Content is being merged into {doc}`onflash_format_spec` as
part of the v1.0.0 documentation restructure (PR 4). This URL stays
reachable until the merge lands.
```

This document describes the runtime policy enforcement implemented in the
UBI secure backend: freshness synchronisation, event callbacks, key-version
refcount tracking, usage budgets, and the sticky crypto read-only mode.

## Freshness Sync

After every commit-visible mutation (volume create/resize/remove, LEB write,
LEB map, PEB erase) the backend calls `sync_freshness` according to the
cadence configured by `CONFIG_UBI_CRYPTO_FRESHNESS_SYNC_DELTA`:

- **delta = 0** (default): sync after every mutation.
- **delta > 0**: sync every N mutations.

If `sync_freshness` returns a non-zero error code, the backend:

1. Emits `UBI_CRYPTO_EVENT_FRESHNESS_SYNC_FAILURE` via the `event_cb`.
2. Optionally enters sticky crypto read-only when
   `CONFIG_UBI_CRYPTO_STRICT_RO_ON_FRESHNESS_SYNC_FAILURE=y`.

## Event Callback and Verdicts

Every security-relevant event is delivered through the application-provided
`event_cb`. The callback returns one of:

| Verdict | Meaning |
|---------|---------|
| `UBI_CRYPTO_EVENT_CONTINUE` | Normal operation continues. |
| `UBI_CRYPTO_EVENT_ENTER_READ_ONLY` | Sticky crypto read-only: all subsequent mutations are rejected with `-EROFS`. Reads remain functional. |

### Event Types

| Event | Trigger |
|-------|---------|
| `AUTH_FAILURE` | AEAD authentication failed during LEB read (EC, VID, or data domain). |
| `FORMAT_VIOLATION` | Post-AEAD plaintext has valid authentication but invalid structure (size mismatch). |
| `KEY_VERSION_NOT_ALLOWLISTED` | On-flash object carries a key version absent from the runtime allowlist. |
| `KEY_VERSION_UNAVAILABLE` | `get_key_id` callback failed — key material not available for a key version. |
| `ROLLBACK_POLICY_MISMATCH` | On-flash freshness lags behind the trusted store at attach time. |
| `FRESHNESS_SYNC_FAILURE` | `sync_freshness` callback returned non-zero. |
| `RNG_FAILURE` | Platform RNG could not produce a fresh salt for a secure write. |
| `KEY_ROTATE_SOON` | LEB write/byte budget crossed soft threshold (`ROTATE_SOON_PCT`, default 80%). |
| `KEY_ROTATE_NOW` | LEB write/byte budget crossed hard threshold (`ROTATE_NOW_PCT`, default 95%). |
| `KEY_RETIRABLE` | All on-flash PEBs authenticated with a non-write-active key version have been erased. The key material can be safely destroyed. |

## Sticky Crypto Read-Only

When `read_only_crypto` is set (by an event callback verdict or by a strict-RO
Kconfig policy), the central mutation gate blocks **all** mutation classes:

- `UBI_MUT_RESERVED_METADATA` (volume create/resize/remove)
- `UBI_MUT_DATA_PATH` (LEB write/map/unmap)
- `UBI_MUT_MAINTENANCE` (PEB erase)

The flag persists until `ubi_device_deinit`. It is independent of the degraded
read-only flag which only blocks reserved-metadata mutations.

## Key-Version PEB Refcount

During attach, the init scan counts refcounts per data PEB: one for each
EC header plus two for each VID-bearing PEB (VID header + LEB data record).
Reserved PEBs are also counted: every reserved PEB contributes one secure
device header plus one secure volume header per existing volume
(`nr_res_pebs * (1 + vol_count)`), so a key version is only retirable
once both its data-PEB objects and its reserved-PEB objects have been
replaced.

At runtime:

- **Write (VID commit)**: increment refcount by 2 for the write-active key
  version (VID header + LEB data objects).
- **Erase**: decrement refcount for the old EC key version (×1), plus VID key
  version (×2) if the PEB had a VID header. Increment by 1 for the
  write-active key version (new EC header written after erase).
- **Reserved metadata commit** (`volume_create` / `volume_resize` /
  `volume_remove`): the (kv, vol_count) contribution of the new state is
  added before the old state's contribution is released ("inc-first /
  dec-last").  This avoids transiently dropping the active kv's refcount
  to zero, which would otherwise spuriously fire `KEY_RETIRABLE`.
- **KEY_RETIRABLE**: emitted when a non-write-active key version's refcount
  reaches zero.

## VID-Domain Counter Floor on Key Rotation

The authenticated `vid_next_counter_floor` field in the secure device
header records the next unused VID-domain AEAD counter for the current
write-active key version.  When attach detects that
`requested_write_key_version` differs from the on-flash write-active key
version, the eager reserved-PEB upgrade restarts the floor at zero:
`K_volume_identifier[new_kv]` is a fresh HKDF child key, so its 48-bit
nonce range is unused under the new version.  Reattaching with the same
key version preserves the monotonic floor.

## LEB Usage Budget

Each LEB write tracks:

- `leb_write_counter` — number of AEAD encrypt operations per `{key_version, volume_id}`.
- `leb_total_auth_bytes` — cumulative authenticated bytes per `{key_version, volume_id}`.

**Pre-write check (§14.2)**: before any flash mutation, the backend projects
the post-write counter and byte usage. If either would cross `ROTATE_NOW_PCT`,
the write is rejected with `-ENOSPC` and `KEY_ROTATE_NOW` is emitted. If the
48-bit nonce counter would overflow, the write is rejected with `-EOVERFLOW`.

**Post-write check**: after each successful LEB commit, usage percentages are
computed against the Kconfig budgets (`UBI_CRYPTO_LEB_WRITE_BUDGET`,
`UBI_CRYPTO_LEB_TOTAL_AUTH_BYTES_BUDGET`) and `KEY_ROTATE_SOON` or
`KEY_ROTATE_NOW` events are emitted when thresholds are crossed.

## Metadata Usage Budget

In addition to the per-`{key_version, volume_id}` LEB budget, each
metadata-bearing AEAD record class is enforced under the active
`write_active_key_version`:

- **DEV** — encrypted device-header records (one per reserved-PEB commit).
- **VOL** — encrypted volume-header records (`vol_count` per reserved-PEB commit).
- **EC** — secure erase-counter headers written on every PEB erase.
- **VID** — volume-ID headers written on every LEB write.

DEV and VOL share the same on-flash counter (`next_dev_hdr_counter`); EC
uses `next_ec_counter`; VID uses `next_vid_counter`. The per-record
authenticated-byte sizes are derived from existing AAD/plaintext/record-size
macros and are `BUILD_ASSERT`-locked in `ubi_secure_budget.c`.

**Pre-commit check**: before any flash mutation, the backend projects the
post-commit counter and authenticated-byte total. If either crosses
`ROTATE_NOW_PCT` of `UBI_CRYPTO_METADATA_COUNTER_BUDGET` /
`UBI_CRYPTO_METADATA_TOTAL_AUTH_BYTES_BUDGET`, `KEY_ROTATE_NOW` is emitted,
sticky `read_only_crypto` is set, and the operation is rejected with
`-ENOSPC` (or `-EROFS` if the gate already trips on a subsequent call).

**Post-commit check**: after the on-flash counter has been bumped, usage
percentages are evaluated and `KEY_ROTATE_SOON` or `KEY_ROTATE_NOW` is
emitted when thresholds are crossed.

**Budget reset on rotation**: per-domain RAM-only "budget bases" are captured
during `ubi_device_init`. When a successful rotation occurs (eager rotation
at attach because `requested_write_key_version` differs from the on-flash
key version), the bases are set to the current counter values so all
subsequent writes count from zero under the new HKDF child keys. When the
write-active kv is unchanged across a reattach, the bases stay at zero so
the cumulative budget under that kv carries forward.

## Error Propagation

Internal crypto error codes (`UBI_SECURE_ENORAND`, `UBI_SECURE_ENOKEY`,
`UBI_SECURE_EFORMAT`) propagate from low-level functions through the I/O
layer to callers that hold the `ubi_device*`. Those callers classify the
error and emit the appropriate event via the helpers in `ubi_secure_event.h`.

## Read-Path Allowlist

During `ubi_secure_leb_read`, both the EC header and VID header key versions
are checked against the runtime `allowed_key_versions` policy. If either
key version is absent from the allowlist, the read is rejected with `-EACCES`
and `KEY_VERSION_NOT_ALLOWLISTED` is emitted.

## Zeroization

All stack-local plaintext buffers (EC, VID, LEB decrypt outputs) and
heap-allocated scratch buffers are wiped via `ubi_secure_zeroize()` — a
volatile-qualified byte-by-byte memset that is not subject to dead-store
elimination — before returning or freeing.

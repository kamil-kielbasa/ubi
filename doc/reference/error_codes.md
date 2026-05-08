# Error Codes

Every UBI public API function returns either `0` on success or a
**negative** standard `errno` value on failure. This page enumerates
each value: when UBI returns it, what it means at the application
layer, and how the application is expected to react.

The table is the lookup view; the sections below add per-code
recovery notes for the cases where "what to do" is not obvious.

```{contents}
:local:
:depth: 2
```

## Lookup table

| Code | Returned by | Meaning | Recommended reaction |
|------|------------|---------|---------------------|
| `-EINVAL` | every public function | NULL pointer, bad parameter, geometry mismatch (`vol_id` does not exist as int range, `lnum` ≥ `leb_count`, `len` past end of LEB, malformed `crypto_cfg`, …) | Programmer error — fix the caller. Do not retry. |
| `-ENOENT` | `ubi_leb_*`, `ubi_volume_*`, `ubi_volume_get_info` | Volume with given `volume_id` does not exist (was never created or has been removed); LEB number out of the volume's range | Treat as "not found". Re-create the volume or skip the operation. |
| `-EEXIST` | `ubi_volume_create` | A volume with the requested **name** already exists with a *different* configuration (type or `leb_count` differs) | Decide intent: either accept the existing config or remove + recreate. Same name + identical config returns `0` (idempotent). |
| `-ENOSPC` | `ubi_leb_write`, `ubi_leb_map`, `ubi_volume_create`, `ubi_volume_resize`, secure budget exhaustion | No free PEB available, or `vol_id_watermark` exhausted, or per-key budget reached its hard limit | Run `ubi_device_erase_peb()` to reclaim dirty PEBs. If still `-ENOSPC`, the partition is genuinely full or (secure mode) a key rotation is required. |
| `-EROFS` | every mutating call when the device is in degraded read-only mode | Reserved-PEB redundancy is lost (only 1 active reserved PEB, no spares) **or** the secure backend has latched a strict-RO policy | Reads remain available. Call `ubi_device_erase_peb()` periodically — it attempts self-healing by rewriting the bad reserved bank. If recovery succeeds, the flag clears automatically. |
| `-EIO` | every flash-touching call | Underlying `flash_area_*` returned an error (read, write, or erase failed) | Retry once. Persistent `-EIO` indicates physical wear or a hardware fault; the affected PEB will be marked bad on the next operation. |
| `-ENOMEM` | every allocating call | Memory backend is exhausted: static slab pools full (raise `CONFIG_UBI_MAX_NR_OF_*`) or `k_malloc()` returned NULL (heap backend) | Programmer error in capacity planning. Re-tune the static pools or the application heap. |
| `-EBUSY` | `ubi_device_init` | Another `ubi_device *` handle for the same `partition_id` is already active | Re-use the existing handle, or call `ubi_device_deinit()` on the previous owner first. |
| `-ECANCELED` | `ubi_volume_resize` | Operation is not applicable: resize on a static volume, or `leb_count` unchanged | Not an error per se. The caller asked for a no-op. |
| `-ENOTSUP` | `ubi_device_init` (secure mode) | A required secure-mode platform capability (PSA `AES-128-CCM`, `HKDF-SHA-256`, or CSPRNG) is not available | Adjust Kconfig: enable PSA Crypto and entropy. Secure UBI cannot be brought up without these. |
| `-EOVERFLOW` | secure I/O paths, secure `volume_resize` | An on-flash counter (per-LEB `record_seq`, per-key `aead_invocations`, etc.) has reached its representable maximum | Trigger a key rotation; the new write-active key resets the counters. |
| `-EFBIG` | secure LEB write | The supplied payload is larger than the secure LEB capacity (LEB size minus AEAD overhead, or chunk size when chunked layout is enabled) | Reduce the write size or enable / re-tune `CONFIG_UBI_CRYPTO_LEB_CHUNKED` and `CONFIG_UBI_CRYPTO_LEB_CHUNK_SIZE`. |
| `-EBADMSG` | secure read paths, secure attach, reserved-PEB scan | Authentication failure (AEAD tag mismatch, invalid header magic, CRC mismatch on a secure record) | Treat the affected record as untrusted. The secure backend will route the failure through the event callback (`UBI_CRYPTO_EVENT_DECRYPT_FAIL`); the application's policy decides whether to drop the LEB, latch read-only, or attempt recovery. |
| `-EACCES` | secure attach (`ubi_device_init` with `crypto_cfg`); plain `leb_unmap` on a static volume | A freshness / policy callback rejected the attach; or an attempt was made to unmap a LEB on a static volume | Attach: investigate `check_freshness` — a rollback was reported. Unmap: forbidden by design on static volumes. |

## Per-code notes

### `-ENOSPC` is overloaded

Three distinct conditions surface as `-ENOSPC`:

1. **No free PEB** for a write — call `ubi_device_erase_peb()` until
   it returns `-ENOENT` (no dirty PEBs left); if writes still fail
   with `-ENOSPC` the partition is full at the *physical* level.
2. **Volume ID watermark exhausted** — the device has created
   `UINT32_MAX` volumes over its lifetime (volume IDs are
   monotonic and never reused). This is unrecoverable without a full
   reformat.
3. **Per-key budget reached `ROTATE_NOW_PCT`** *(secure mode only)* —
   the secure backend refuses further writes against a key that has
   reached its hard usage threshold. Provide a new key version via
   `get_key_id` and either bump `requested_write_key_version` in
   `crypto_cfg` or re-attach with the rotated allowlist.

### `-EROFS` and self-healing

`-EROFS` does not mean the device is permanently broken. The
maintenance call `ubi_device_erase_peb()` is intentionally allowed
in read-only mode: after its dirty-PEB cycle it tries to rewrite the
bad reserved bank from the surviving copy. If that succeeds, the
internal `read_only_degraded` flag clears and the next mutating call
will succeed. See {doc}`/architecture/plain_architecture` § *Read-Only
Degraded Mode* for the full mutation gate.

### `-EBADMSG` is a security event, not a transient I/O error

Never treat `-EBADMSG` as something to retry. It means a record
authentication tag did not validate — either the flash content was
tampered with, or a key version mismatch slipped through. The secure
backend always emits a corresponding `UBI_CRYPTO_EVENT_*` to the
application's `event_cb` before returning. Use that event (not the
errno) as the trigger for security policy.

### `-EBUSY` means "single handle per partition"

UBI enforces one active `ubi_device *` per flash partition. The
guard is set in `ubi_device_init()` and released in
`ubi_device_deinit()`. Tests that rapidly cycle init/deinit can use
`ubi_test_partition_guard_reset()` (gated by
`CONFIG_UBI_TEST_API_ENABLE`) to reset the guard between iterations.

## Cross-reference

- {doc}`/reference/api` — the per-function Doxygen reference; each
  function's `\retval` lines list the codes it can actually return.
- {doc}`/architecture/secure_overview` § *Events and read-only mode* —
  how `-EBADMSG`, `-EACCES`, and the strict-RO policies interact.
- {doc}`/guide/secure_workflow` § 4.4 — handling
  `UBI_CRYPTO_EVENT_*` callbacks (the security counterpart to the
  errno surface).

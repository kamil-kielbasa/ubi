# Secure UBI Workflow

**What this page covers:** How to wire Secure UBI into your
application — when to enable it, what you have to provide, the exact
callback contracts, key rotation as a workflow, event handling, and
retirement.

**Prerequisites:** {doc}`/architecture/secure_overview`. For the byte-level format
and recovery rules, see {doc}`/reference/onflash_format_spec`.

**After reading this:** You will know exactly what to put in your
`crypto_cfg`, what each callback must do, and how to drive a key
rotation end-to-end without losing data or spuriously retiring keys.

---

## 1. When to enable Secure UBI

Enable Secure UBI when **any** of the following is true:

- the device contains user data that must be protected at rest;
- the device contains application data whose integrity matters
  (configuration, audit logs, billing counters);
- you need to detect tampering with raw flash;
- product policy requires authenticated key rotation and retirement.

**Do not** enable Secure UBI when:

- the partition holds only ephemeral data that survives no power
  cycle and tolerates corruption;
- the platform cannot supply PSA Crypto with `AES-128-CCM`,
  `HKDF-SHA-256`, and a CSPRNG (Secure UBI is unsupported there);
- there is no path for a **device-unique** root secret (a serial
  number, MAC, or UUID is **not** acceptable as the IKM source — see
  {doc}`/architecture/secure_overview` §3).

Cost rule of thumb (full numbers in {doc}`/architecture/plain_architecture`
§ Resource Usage): expect ~50 KB of additional flash from the PSA /
Mbed TLS code Secure UBI pulls in, and a few hundred bytes of extra
RAM per device / volume.

## 2. Prerequisites checklist

Before your first Secure UBI build, make sure all of the following
are in place:

| Prerequisite | Where it lives | How to verify |
|--------------|---------------|---------------|
| `CONFIG_UBI_CRYPTO=y` | Kconfig | Build picks up the secure backend |
| PSA Crypto with `AES-128-CCM` and `HKDF-SHA-256` | Kconfig (`CONFIG_PSA_*`) | `psa_aead_*` and `psa_key_derivation_*` link |
| Platform CSPRNG | Kconfig (`CONFIG_ENTROPY_*`) | `psa_generate_random()` succeeds at boot |
| Device-unique root key (≥ 256-bit) provisioned as a PSA key | Provisioning tool / secure element | `psa_get_key_attributes()` returns the expected lifetime / type |
| `get_key_id` callback that resolves your `key_version`s | Application code | Returns `PSA_KEY_ID_NULL` only for un-provisioned versions |
| Allowlist of accepted `key_version`s | Application config | Matches the key versions actually present in PSA |
| Freshness store (NVRAM / Zephyr Settings / TEE-backed counter) | Application code | Survives reboot and is itself integrity-protected |
| Event callback wired to your security policy | Application code | Logs / escalates / triggers rotation as appropriate |

## 3. Setting up `crypto_cfg`

Secure UBI is selected at runtime by passing a non-`NULL`
`crypto_cfg` to `ubi_device_init()`. The exact fields are documented
in {doc}`/reference/api`; the contract for **each** field is:

```c
struct ubi_crypto_cfg cfg = {
    /* Versioned root keys (4.1) */
    .get_key_id           = my_get_key_id,
    .get_key_id_user_ctx  = &my_ctx,

    /* Allowlist (4.2) */
    .allowed_key_versions     = (const uint8_t[]){ 1, 2 },
    .allowed_key_versions_len = 2,
    .requested_write_key_version = 2,   /* preferred write-active */

    /* Freshness store (4.3) */
    .check_freshness          = my_check_freshness,
    .sync_freshness           = my_sync_freshness,    /* optional */
    .freshness_user_ctx       = &my_ctx,

    /* Event callback (4.4) */
    .event_cb                 = my_event_cb,
    .event_user_ctx           = &my_ctx,
};

struct ubi_device *ubi = NULL;
int err = ubi_device_init(&flash, &cfg, &ubi);
```

If `cfg` is `NULL`, Secure UBI is **not** selected — UBI attaches in
plain mode. Once a partition is formatted in one mode, attempting to
attach in the other mode is a hard error (no silent reformat, no
in-place migration).

## 4. Callback contracts

### 4.1 `get_key_id` — PSA key provider

**Signature.** `psa_key_id_t get_key_id(uint8_t key_version, void *ctx)`.

**Contract:**

- For every `key_version` in your allowlist, return the corresponding
  PSA key identifier. The key must be importable for HKDF derivation.
- Return `PSA_KEY_ID_NULL` for un-provisioned or destroyed versions —
  Secure UBI then emits `KEY_VERSION_UNAVAILABLE`.
- Must be deterministic and side-effect-free.

### 4.2 Allowlist

**Field.** `allowed_key_versions[]` (8-bit values), with
`allowed_key_versions_len`.

**Contract:**

- Each entry is one allowed `key_version`. Duplicates are invalid input.
- Any on-flash `key_version` not in this array is rejected and
  `KEY_VERSION_NOT_ALLOWLISTED` is emitted.
- The optional `requested_write_key_version` must be present in the
  allowlist; downgrade attempts are rejected.

### 4.3 Freshness store

**Signatures.**

```c
int  check_freshness(const struct ubi_freshness_descriptor *d, void *ctx);
int  sync_freshness (const struct ubi_freshness_descriptor *d, void *ctx);
```

**Contract for `check_freshness`** (called once at attach time):

- Compare the authenticated `(device_revision, global_sqnum)` Secure
  UBI hands you against the values in your trusted store.
- Return `0` to accept; non-zero to reject and force read-only or
  init failure (per Kconfig policy).
- If your store is empty (first boot), persist what Secure UBI gave
  you and return `0`.

**Contract for `sync_freshness`** (optional; called after each
commit-visible mutation, throttled by
`CONFIG_UBI_CRYPTO_FRESHNESS_SYNC_DELTA`):

- Persist the new `(device_revision, global_sqnum)` to your trusted
  store before returning.
- A non-zero return triggers `FRESHNESS_SYNC_FAILURE` and — when
  `CONFIG_UBI_CRYPTO_STRICT_RO_ON_FRESHNESS_SYNC_FAILURE=y` — sticky
  read-only.

If `sync_freshness` is omitted, freshness updates only happen the
next time `check_freshness` is called (i.e., the next attach).

### 4.4 Event callback

**Signature.** `enum ubi_crypto_event_verdict event_cb(enum ubi_crypto_event ev, const struct ubi_crypto_event_info *info, void *ctx)`.

**Contract:**

- Always return either `UBI_CRYPTO_EVENT_CONTINUE` or
  `UBI_CRYPTO_EVENT_ENTER_READ_ONLY`.
- Returning `ENTER_READ_ONLY` is **sticky** for the rest of the
  attach session — only `ubi_device_deinit` clears it.
- Your verdict cannot override mandatory rejections that Secure UBI
  must enforce regardless (RNG failure, nonce overflow, missing
  key material): the operation is already rejected by the API
  return code; the verdict only controls whether *future* writes
  may continue.

The event types and what each one means are listed in
{doc}`/reference/onflash_format_spec` chapter 21 (Runtime policy).

## 5. Key rotation workflow

### 5.1 Lazy rotation (default; recommended)

1. **Provision a new key** (`v_new`) via your platform's provisioning
   tool. Add it to PSA. Update the allowlist in your firmware to
   include both `v_old` and `v_new`. Set
   `requested_write_key_version = v_new`.
2. **Reboot or reattach.** Secure UBI detects the new requested
   write-active key, performs an eager reserved-PEB upgrade, and
   begins encrypting **new** writes under `v_new`.
3. **Let normal traffic age the old objects out.** Existing live
   mappings stay under `v_old` until they are overwritten or the
   PEB is reclaimed. Secure UBI tracks this via the per-version
   refcount.
4. **Wait for `KEY_RETIRABLE` for `v_old`.** When the refcount hits
   zero, Secure UBI emits the event. Only **then** is it safe to
   remove `v_old` from PSA.
5. **Tighten the allowlist** (drop `v_old`) and **destroy the PSA
   key** for `v_old`.

### 5.2 Forced rotation (compromise response)

If `v_old` is suspected to be compromised, lazy rotation alone is
**not** sufficient — old EC headers, dirty data, and stale reserved
generations keep the compromised key materially in play. Add these
steps to the lazy workflow:

- After step 2, call `ubi_device_erase_peb()` aggressively in a
  workqueue to reclaim every dirty PEB.
- Rewrite all live mappings under `v_new` (application-level
  `read → write` of every LEB).
- Only after both of the above does step 4 (`KEY_RETIRABLE` for
  `v_old`) actually mean the compromised key has no on-flash
  reference left.

The four operational states (soft rotation → live rewrite completed →
media scrub completed → key retired) are described in
{doc}`/architecture/secure_overview` §5 and specified in {doc}`/reference/onflash_format_spec`
§13.8.

### 5.3 Watching the budgets

Secure UBI emits `KEY_ROTATE_SOON` (default 80% of the active key's
budget) and `KEY_ROTATE_NOW` (default 95%) on its own. Treat
`KEY_ROTATE_SOON` as the trigger to **start** the rotation workflow;
hitting `KEY_ROTATE_NOW` will reject further writes until you rotate.

## 6. Event handling

A minimal but correct event handler:

```c
static enum ubi_crypto_event_verdict
my_event_cb(enum ubi_crypto_event ev,
            const struct ubi_crypto_event_info *info,
            void *ctx)
{
    switch (ev) {
    case UBI_CRYPTO_EVENT_AUTH_FAILURE:
    case UBI_CRYPTO_EVENT_FORMAT_VIOLATION:
    case UBI_CRYPTO_EVENT_ROLLBACK_POLICY_MISMATCH:
        /* Tamper-suspected. Lock down. */
        log_security(ev, info);
        return UBI_CRYPTO_EVENT_ENTER_READ_ONLY;

    case UBI_CRYPTO_EVENT_RNG_FAILURE:
    case UBI_CRYPTO_EVENT_FRESHNESS_SYNC_FAILURE:
        /* Operational failure. Stop writing until next attach. */
        log_warn(ev, info);
        return UBI_CRYPTO_EVENT_ENTER_READ_ONLY;

    case UBI_CRYPTO_EVENT_KEY_ROTATE_SOON:
        /* Schedule rotation work; keep running. */
        schedule_key_rotation(info);
        return UBI_CRYPTO_EVENT_CONTINUE;

    case UBI_CRYPTO_EVENT_KEY_ROTATE_NOW:
        /* Try one more time to rotate before the next write. */
        kick_key_rotation_now(info);
        return UBI_CRYPTO_EVENT_CONTINUE;

    case UBI_CRYPTO_EVENT_KEY_RETIRABLE:
        /* Safe to destroy the corresponding PSA key now. */
        purge_psa_key(info->key_version);
        return UBI_CRYPTO_EVENT_CONTINUE;

    case UBI_CRYPTO_EVENT_KEY_VERSION_NOT_ALLOWLISTED:
    case UBI_CRYPTO_EVENT_KEY_VERSION_UNAVAILABLE:
        /* Likely a downgrade or provisioning gap. */
        log_warn(ev, info);
        return UBI_CRYPTO_EVENT_ENTER_READ_ONLY;
    }
    return UBI_CRYPTO_EVENT_CONTINUE;
}
```

Two reminders:

- `ENTER_READ_ONLY` is **sticky** until `deinit` — make sure your
  application can recover (typically by closing and re-attaching).
- The verdict does not override mandatory rejections; the *current*
  failing operation always returns its specific error code as well.

## 7. Retirement

A key is **retirable** only when no authenticated object on flash
still references it. The full chain (live LEBs, dirty PEBs, stale EC
headers, stale reserved generations) is tracked by Secure UBI as a
per-version refcount. The application must:

1. Wait for `KEY_RETIRABLE` for the version it wants to remove.
2. Drop the version from `allowed_key_versions[]` on the next
   reattach.
3. Destroy the PSA key (e.g., `psa_destroy_key()`).

There is no API to "force" retirement of a key with non-zero
refcount; doing so would silently break recovery. Use the forced
rotation steps in §5.2 to actually drive the refcount to zero.

## 8. What's next

- {doc}`/reference/onflash_format_spec` — chapters 13 (key lifecycle), 14
  (events), 19 (lifecycle), 20 (recovery), 21 (runtime policy) for
  the normative rules behind every paragraph above.
- {doc}`/guide/cookbook` — runnable recipes: provisioning, lazy rotation,
  forced rotation, freshness store on Zephyr Settings (lands in
  PR 5).
- {doc}`/project/test_strategy` § *ZTEST traceability for Secure UBI* — see
  exactly which test exercises which behaviour.

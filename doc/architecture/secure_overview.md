# Secure Architecture: Overview

**What this page covers:** Secure UBI in a developer-friendly form —
what it does, how the key hierarchy works, the threat model, the
contract between Secure UBI and your application, and the four-state
key lifecycle.

**Prerequisites:** {doc}`/getting_started/what_is_ubi` and {doc}`/architecture/plain_architecture`.

**After reading this:** You will understand whether Secure UBI fits
your product. To wire it into an application, continue to
{doc}`/guide/secure_workflow`. For the byte-level normative spec, see
{doc}`/reference/onflash_format_spec`.

---

## 1. What Secure UBI is

A UBI device runs in exactly **one** mode: **Plain** or **Secure**. In
Secure mode, every commit-visible object on flash is wrapped in
authenticated encryption (AES-128-CCM):

- secure device header,
- secure volume headers,
- secure erase-counter (EC) headers,
- secure volume-identifier (VID) headers,
- secure LEB records (your data).

The plain UBI object model is preserved — wear-leveling, logical
volumes, dual-bank reserved metadata, and `sqnum`-based recovery
continue to work the same way. Only the on-flash representation,
keying, authentication, and recovery rules change.

In one paragraph: **Secure UBI gives you confidentiality and
authenticity for both your data and UBI's own metadata, plus a
production-grade key lifecycle (allowlist, rotation, retirement) and
an application-visible freshness hook for rollback / replay
defence — built on top of plain UBI's wear-leveling and crash
recovery.**

```{mermaid}
graph TD
    App["Application<br/>(filesystem / database / freshness store)"]
    UBI["UBI Secure<br/>(volumes, wear-leveling, AEAD wrappers,<br/>key lifecycle, freshness, anchors)"]
    PSA["PSA Crypto<br/>(AES-128-CCM, HKDF-SHA-256, RNG)"]
    HW["Physical Flash"]

    App --> UBI
    UBI --> PSA
    UBI --> HW
```

Secure UBI is **PSA-only** — it consumes PSA Crypto (`AES-128-CCM`,
`HKDF-SHA-256`) and the platform RNG. It never receives raw root-key
bytes.

## 2. Threat model

| Asset                     | Protected against                               | Not protected against                             |
|---------------------------|-------------------------------------------------|---------------------------------------------------|
| User data on flash        | Off-line read, modification, relocation        | Live memory dumps after a successful attach       |
| UBI device / volume metadata | Off-line read, modification, relocation     | (same)                                            |
| Per-volume isolation      | Cross-volume key reuse                         | Compromise of the per-key root `IKM[v]`           |
| On-flash structure        | Forgery without an accepted key version        | Rollback to an earlier authenticated state, unless caught by your freshness store |
| Replay of an authentic record to a different physical PEB or LEB | AAD binding rejects mismatched location / identity | n/a |

What an attacker with raw flash access **cannot** do without an
accepted `IKM[v]`:

- read meaningful UBI metadata or user payload;
- modify any authenticated field;
- move ciphertext to another physical eraseblock, volume, or logical
  number and have it accepted;
- replay older authenticated state without also defeating your
  application's freshness policy.

What Secure UBI **does not** claim:

- It does not stop a hardware monotonic counter from being absent.
  Complete anti-rollback still requires an external trusted freshness
  store or equivalent trust anchor.
- It does not protect data once it has been read into application
  memory — that is the application's job.
- It does not migrate plain media to secure media in place
  (out of scope; must be an explicit offline procedure).

## 3. Key hierarchy

Secure UBI derives every working key from a single per-version root,
`IKM[v]`. The root is referenced by a **PSA key identifier**; raw root
bytes never enter the UBI API.

```{image} ../img/key_hierarchy.svg
:alt: Secure UBI key hierarchy — IKM[v] is HKDF-extracted into PRK[v], which is HKDF-expanded into per-domain child keys (device, volume, EC, VID, LEB); the LEB key is further bound to the durable volume_id.
:align: center
:width: 720px
```

Properties to remember:

- **Domain separation.** Each domain (DEV / VOL / EC / VID / LEB) uses
  a distinct child key derived with a fixed HKDF label.
- **Per-volume LEB key.** `K_leb[v][volume_id]` is unique per volume,
  so cross-volume key reuse is impossible.
- **`volume_id` is durable.** It is never reused for the lifetime of
  one formatted device, so Secure UBI can use it as a stable
  cryptographic identity.
- **Versioned roots.** Every authenticated object carries an 8-bit
  `key_version`. The application supplies an allowlist; on-flash
  versions outside the allowlist are rejected.
- **Root requirements (mandatory).** `IKM[v]` must be **at least 256
  bits**, **device-unique**, and provided as a **PSA key**. Identifiers
  such as serial number, MAC, or UUID are **not** acceptable.

## 4. Application contract

Secure UBI delegates four things to your application. They are
non-optional for any non-trivial deployment. The detailed signatures,
contracts, and verdict semantics are the canonical
{doc}`/guide/secure_workflow` § 4 *Callback contracts*; this section is
the at-a-glance map.

| What you provide | Why | Detail |
|---|---|---|
| **PSA key provider** — `get_key_id(key_version) -> psa_key_id_t` | Look up the PSA key holding `IKM[v]` for any allowlisted version Secure UBI sees on flash. | {doc}`/guide/secure_workflow` § 4.1 |
| **Key allowlist** — `allowed_key_versions[]` | Explicit list of 8-bit versions Secure UBI may authenticate; everything else triggers `KEY_VERSION_NOT_ALLOWLISTED`. | {doc}`/guide/secure_workflow` § 4.2 |
| **Freshness store** — `check_freshness` (mandatory) and `sync_freshness` (optional) | Without a freshness store Secure UBI still gives you authenticated encryption, but cannot detect rollback to an earlier authentic state. | {doc}`/guide/secure_workflow` § 4.3 |
| **Event callback** — `event_cb(event) -> verdict` | Receive every security-relevant event (auth failures, allowlist violations, RNG failures, rotation thresholds, retirement) and decide whether to keep going or latch read-only. | {doc}`/guide/secure_workflow` § 4.4 |

## 5. Key lifecycle

A key version moves through four states. Secure UBI surfaces the
**`KEY_RETIRABLE`** transition; the others are observable from the
application side.

```{mermaid}
flowchart LR
    A["1. Active write-key"] --> B["2. Soft rotation<br/>new writes use a newer key version"]
    B --> C["3. Live rewrite completed<br/>all live mappings rewritten under the new key"]
    C --> D["4. Media scrub completed<br/>stale dirty / free-PEB EC objects gone"]
    D --> E["5. KEY_RETIRABLE<br/>refcount reaches zero"]
    E --> F["Application: remove from allowlist<br/>and purge PSA key material"]
```

Rules that follow from this lifecycle:

- The **write-active key version is monotonic** — it must never move
  back to an older value on the same formatted device.
- Changing the write-active key does **not** instantly retire the old
  key. Old EC headers, dirty data, and stale reserved generations
  keep the old key alive until they are physically erased or
  overwritten.
- Secure UBI tracks an **on-flash refcount per key version** and
  emits `KEY_RETIRABLE` when it reaches zero — that is the only
  point where it is safe for the application to destroy the
  PSA-managed root.
- Secure UBI also tracks **usage budgets** under the active key (LEB
  writes, authenticated bytes, metadata records). When usage crosses
  the soft threshold (`KEY_ROTATE_SOON`, default 80%) or the hard
  threshold (`KEY_ROTATE_NOW`, default 95%), Secure UBI signals you
  via the event callback. Hitting `KEY_ROTATE_NOW` rejects further
  writes until rotation.
- For **compromise response**, lazy rewrite alone is **not**
  sufficient: if you need immediate retirement, accelerate reclaim,
  scrub stale media state, and only tighten the allowlist after the
  refcount of the compromised version reaches zero.

## 6. What's next

- {doc}`/guide/secure_workflow` — when to enable Secure UBI, the prerequisites
  you need in place (PSA, RNG, allowlist, freshness store), the exact
  callback contracts, key rotation as a workflow, and event handling.
- {doc}`/reference/onflash_format_spec` — the byte-level normative specification
  (record layouts, AAD, nonce rules, on-flash counters, lifecycle
  invariants, recovery scenarios, runtime policy).
- {doc}`/guide/cookbook` — recipes including "Implementing key rotation with
  PSA" and "Implementing a freshness store with Zephyr Settings".
- {doc}`/project/test_strategy` — the ZTEST traceability tables that show how
  every Secure UBI rule is exercised in regression.

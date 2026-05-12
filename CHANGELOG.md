# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.91.0] - 2026-05-12

### Added

- Two test-only secure hooks, gated by
  `CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION`:
  `ubi_secure_test_get_peb_for_lnum()` resolves the live PEB for a
  `(vol_id, lnum)` mapping, or the hidden anchor PEB when `lnum` is
  `SIZE_MAX`; `ubi_secure_test_read_vid_meta_from_peb()` drives the
  standard EC + VID authentication path against any PEB and surfaces
  the authenticated `leb_write_counter`, `leb_total_auth_bytes` and
  `sqnum`. The two hooks unlock numeric counter-floor assertions in
  upcoming anchor-continuity and stress tests without duplicating the
  AEAD read path.

## [0.90.0] - 2026-05-12

### Changed

- The crypto-related fields on `struct ubi_device` are now grouped into
  three sub-structs (`aead`, `budget_bases`, `freshness`) and the two
  PEB pools are now one struct (`free_pool` / `dirty_pool`, each
  carrying its rbtree and its cached count side by side). The shape of
  the on-flash format does not change; only the names used inside the
  backend do (e.g. `ubi->next_vid_counter` -> `ubi->aead.next_vid`,
  `ubi->free_peb_count` -> `ubi->free_pool.count`). Public
  `struct ubi_device_info` is unchanged so application code keeps
  working as-is.

- The reserved-PEB AEAD counter was renamed from `next_dev_hdr` to
  `next_res_peb` to reflect what it actually tracks: a single
  reserved-PEB commit writes one DEVICE_HEADER record followed by N
  VOLUME_HEADER records and consumes `1 + N` consecutive slots from
  this one monotonic sequence. The old name suggested it was only
  about device headers.

### Added

- `CONFIG_UBI_CRYPTO_SYNC_FRESHNESS_VERIFY` (default `n`). When enabled,
  every successful `sync_freshness` callback is immediately followed by
  a `check_freshness` round-trip against the same snapshot. If
  `check_freshness` then rejects, the runtime emits
  `ROLLBACK_POLICY_MISMATCH` -- catching the case where the application
  acknowledged `sync_freshness` but failed to durably persist the new
  state. Off by default; integrations that already verify in their
  persistence layer pay nothing.

### Documentation

- `struct ubi_dev_secure_meta::reserved0` and
  `struct ubi_crypto_prefix32::reserved` / `::flags` doxygen reads
  "reserved, not used" instead of "zero in v1".
- The two-region scratch buffer in the secure LEB read path
  (ciphertext+tag followed by plaintext) gained an explanatory
  comment -- AEAD forbids in-place decrypt because the tag check spans
  the whole ciphertext, so both buffers must be live at the same time.
- The 2x `dec_and_check(vid_kv)` after dirty-PEB recovery in
  `ubi_secure_runtime.c` now carries a comment: a data PEB
  authenticated under one key-version contributes two on-flash objects
  (VID header + LEB payload) and both are reclaimed together with the
  PEB, so the per-kv refcount must drop by two.

## [0.89.0] - 2026-05-12

### Changed

- Stack byte buffers used to build an AEAD nonce are now always
  zero-initialized, closing a path where a partial fill could have
  carried stack garbage into the chunked-LEB nonce.

- Header-prefix and device-metadata structs are zeroed exactly once,
  via their compound-literal initializer. The duplicate
  `memset(...reserved..., 0, ...)` that followed each initializer
  has been removed as dead code. The one `memset` that still exists
  (`ubi_secure_dev_meta_deserialize()`) is intentional -- it discards
  bytes read from flash, not initializer slack.

### Documentation

- `CODE_STYLE.md` now spells out the conventions the secure backend
  already follows: standalone increments are written `x += 1;` /
  `x -= 1;` (embedded post-increment and for-loop induction stay as
  they are), stack byte buffers must be `= { 0 }`-initialized, a
  compound-literal initializer is not to be followed by a redundant
  `memset` of its `reserved` fields, and the `__ASSERT_NO_MSG`
  precondition block is separated from the rest of the body by a
  blank line. The secure backend was swept to match.

## [0.88.0] - 2026-05-12

### Changed

- Usage-percentage values now carry the same type end-to-end. The
  budget helpers (`ubi_secure_usage_pct()` and friends) returned
  `unsigned int` while the public `ubi_crypto_event.rotation.usage_pct`
  field is `uint8_t`, so every caller wrote a `(uint8_t)` cast at the
  emission site. The helpers now return `uint8_t` directly and the
  casts are gone.

- AEAD entry points reject the under-specified case `aad == NULL &&
  aad_len != 0` at the boundary with `-EINVAL` instead of forwarding
  it to PSA, and `ubi_secure_build_label()` rejects `label_cap == 0`
  before the size check. `ubi_secure_destroy_key()` now logs a warning
  when PSA reports anything other than success or
  `PSA_ERROR_INVALID_HANDLE`, so a key-slot leak no longer disappears
  silently on cleanup paths.

### Renamed

- File-static helpers in `ubi_secure_reserved.c` renamed so the name
  reflects the operation pipeline (`<object>_<steps-in-order>`):
  `authenticate_dev_hdr()` → `dev_hdr_aead_decrypt_unpack()` (AEAD
  decrypt then unpack plaintext into typed structs -- authentication
  is a side-effect of the AEAD tag check, not the primary action);
  `encrypt_dev_hdr()` → `dev_hdr_pack_aead_encrypt()` and
  `encrypt_vol_hdr()` → `vol_hdr_pack_aead_encrypt()` (pack typed
  struct into the plaintext layout, then AEAD encrypt).

- `old_wc` / `old_tab` locals in `ubi_secure_leb.c` →
  `prev_leb_write_counter` / `prev_leb_total_auth_bytes`, matching the
  field names of the cached counter floor they read.

## [0.87.0] - 2026-05-12

### Changed

- Secure-backend symbol names now follow the project's
  `ubi_<module>_<noun>_<verb>` convention so the owning subsystem is
  visible from the identifier alone. Event-, freshness-, budget-,
  policy- and key-family entry points were renamed accordingly; the
  only public-ABI rename is `ubi_secure_get_write_active_key_version`
  → `ubi_secure_key_get_active_version` in `ubi_crypto.h`. Call
  semantics are unchanged.

- `ubi_secure_policy_check_allowlist()` no longer takes a `pnum`
  argument. The allowlist decision is a pure function of
  `(policy, key_version)`; the PEB index was never consulted and its
  presence falsely suggested the decision could vary per-PEB.

- Doxygen and inline comments brought back in sync with the code:
  the post-write budget helpers now document their parameters (the
  pre-write twins already did), the stale "Check LEB usage budgets"
  block above `ubi_secure_usage_pct()` in `ubi_secure_event.h` --
  describing a function deleted in v0.85.0 -- was removed, and the
  duplicate-anchor branch in `scan_map_first()` now explains the
  crash-mid-rewrite scenario that produces two authenticated anchor
  copies and why higher sqnum is the correct tie-break.

## [0.86.0] - 2026-05-12

### Changed

- Secure-backend internal refactor (no behaviour, API, or on-flash format
  change): the hidden per-volume anchor PEB -- the single PEB that carries
  the authenticated upper bound of a volume's LEB AEAD-counter floor across
  power loss -- is now an independent module (`ubi_secure_anchor.{h,c}`)
  with three documented entry points: creation at volume-create time, the
  sole-witness rewrite that must happen before a dirty PEB carrying the
  cached counter floor may be erased, and the one-PEB reserve refill that
  keeps a free PEB available for that rewrite. Previously these three
  responsibilities were scattered across the volume-management and
  device-runtime translation units (one as a public function, one as a
  file-static helper called from two places, one as a public hook on the
  runtime ops header), which obscured the rule that all three operations
  share the same "rewrite-before-erase" invariant on the cached counter
  floor. Grouping them in one unit also makes the boundary between
  hot-path I/O (LEB read/write) and counter-floor maintenance explicit:
  no other secure-backend unit now writes anchor PEBs.

- Counter-floor erase maintenance no longer silently swallows I/O errors.
  When `ubi_secure_try_refill_reserve()` tries to recycle a dirty PEB so
  that the one-PEB anchor-rewrite reserve is restored from one free PEB
  back to two, an erase failure on that PEB is now reported via
  `LOG_WRN`. The call remains best-effort -- the caller has no actionable
  recovery, and the next user-visible write will surface `-ENOSPC` if the
  reserve cannot be restored -- but the failure is no longer invisible to
  the operator: the warning carries the PEB index and the failing return
  code so that a degraded free pool can be diagnosed from logs alone
  instead of inferred from a downstream `-ENOSPC`.

- On-flash naming for the data-PEB VID header was unified with the rest
  of the secure header family. The AAD/plaintext size macros for this
  domain were previously named `UBI_SECURE_DATA_VID_*` while every other
  domain used the on-flash header name (`UBI_SECURE_DEV_HDR_*`,
  `UBI_SECURE_VOL_HDR_*`, `UBI_SECURE_EC_HDR_*`), and the matching AAD
  input struct followed the same odd convention
  (`ubi_secure_data_vid_aad_input`). They are now `UBI_SECURE_VID_HDR_*`
  and `ubi_secure_vid_hdr_aad_input`, so every secure header domain reads
  as `<DOMAIN>_HDR` in code and the on-flash spec, the runtime budget
  helper, and the AAD builders all use the same identifier. No size,
  field offset, or wire-format byte changed.

- `ubi_secure_ser.{h,c}` is now organized by on-flash domain. Common
  helpers (prefix32 serialize/deserialize, counter48 encode/decode) come
  first, then each header domain (DEV, VOL, EC, VID) groups its AAD
  builder together with its secure-meta serialize/deserialize, then LEB
  record and finally LEB chunk (the latter still gated by
  `CONFIG_UBI_CRYPTO_LEB_CHUNKED`). Previously the file mixed builders,
  meta (de)serializers and helpers in the order they had been added,
  which made the per-domain set of operations hard to read at a glance
  and harder still to extend. The header layout mirrors the source file
  one-for-one, with explicit section banners and a top-of-file summary.
  No declaration, definition, or wire-format byte changed; this is a
  pure layout reorganization to make the per-domain authentication
  contract self-evident from the source.

## [0.85.0] - 2026-05-12

### Changed

- Secure-backend internal refactor (no behaviour, API, or on-flash format
  change): the per-record AAD builders in `ubi_secure_ser.{h,c}` now take a
  typed per-domain input struct (`struct ubi_secure_{dev_hdr,vol_hdr,ec_hdr,
  data_vid,leb,leb_chunk}_aad_input`) instead of a long positional argument
  list, so call sites in `ubi_secure_io.c` and `ubi_secure_reserved.c` make
  the AAD-bound fields explicit; the secure LEB I/O entry points
  (`ubi_secure_leb_data_{read,write}{,_chunked}`) now use typed
  `uint8_t *buf` / `const uint8_t *buf` instead of `void *` payload pointers;
  the HKDF label helpers (`ubi_secure_build_label` and
  `ubi_secure_derive_child_key`) are now file-static inside
  `ubi_secure_crypto.c` and no longer part of the module-internal header.

## [0.84.0] - 2026-05-12

### Changed

- Secure-backend internal refactor (no behaviour, API, or on-flash format
  change): the policy allowlist lookup and the EC/VID/LEB key-derivation paths
  share a single helper; the flash-write fault-injection wrapper is consolidated
  in a new `ubi_secure_flash.h` instead of being duplicated across `io.c` and
  `reserved.c`; `UBI_SECURE_MAX_LABEL_SIZE` is derived from the actual on-flash
  label format and bound to the domain-name strings via `BUILD_ASSERT`;
  `ubi_secure_res_peb_scan()` builds its result on a local copy and only
  publishes it to the caller on success; the 48-bit AEAD-counter overflow check
  now lives inside the budget pre-checks (with `KEY_ROTATE_NOW` and sticky
  crypto-RO), removing the redundant manual guard in
  `leb_prepare_new_mapping()`.

## [0.83.0] - 2026-05-12

### Changed

- Secure backend on-flash record sizes are now bound at compile time
  to the C structures they serialize. Any future change to a header
  layout that would shift the byte count of a committed secure record
  (EC, VID, device header, volume header) now fails the build instead
  of silently producing flash images that are incompatible with the
  spec or with already-deployed devices. No behaviour change, no
  on-flash format change.

## [0.82.0] - 2026-05-12

### Changed

- **BREAKING: the secure backend now uses `psa_key_id_t` for key
  identifiers throughout its public API.** Applications that provide
  the `get_key_id` callback (`ubi_crypto_get_key_id_cb_t`) must
  update its second parameter from `uint32_t *` to `psa_key_id_t *`.
  This restores type fidelity with the PSA Crypto API and removes a
  portability assumption that `psa_key_id_t` is always `uint32_t`.
  No on-flash format change, no behavioural change — secure devices
  attach and continue to operate exactly as before.

## [0.81.0] - 2026-05-12

### Security

- **Fix AEAD nonce-uniqueness regression in the secure backend.** The
  previous `leb_recover_old_counters()` helper only consulted the PEB
  currently mapped to the requested `lnum` when recovering counter
  state for the next write. After `write \u2192 unmap \u2192 erase-all-dirty
  \u2192 write` the new mapping restarted `leb_write_counter` at 0 under
  the same HKDF child key, violating AEAD nonce uniqueness for that
  `{key_version, volume_id}` pair. The hidden per-volume anchor PEB
  (\u00a77.9) was designed to preserve continuity but the runtime never
  consulted it on the new-mapping path. Closes a confidentiality and
  integrity break on data written under the affected `kv`.

### Changed

- **Per-volume AEAD counter floor cached in RAM.** `struct ubi_volume`
  now mirrors `(leb_write_counter, leb_total_auth_bytes)` as the strict
  upper bound across all on-flash evidence for the volume. The cache
  is reseeded from the anchor plus every authenticated data PEB during
  attach scan, bumped before each `leb_data_write` (conservative
  nonce reservation), and refreshed on anchor rewrite. The new
  `leb_get_volume_counter_floor()` replaces the buggy per-LEB
  recovery; the historical `leb_recover_old_counters()` symbol is
  removed.
- **`maybe_rewrite_anchor_for_dirty()` is now O(1).** Because
  `leb_write_counter` is strict-monotonic, at most one on-flash PEB
  of a volume carries `vid_meta.leb_write_counter ==
  cached_leb_write_counter`. The witness check therefore compares the
  dirty PEB's `vid_meta` against the cache instead of scanning every
  EBA entry and every other dirty PEB (previously O(N\u00b2) flash reads
  per erase).

### Added

- New test suite `ubi_secure_anchor` (six tests) covering counter
  inheritance after unmap+erase, multi-LEB churn, cold-attach reseed
  from the anchor, non-witness-erase no-op behaviour, 48-bit counter
  saturation (`-EOVERFLOW` + `KEY_ROTATE_NOW`), and a 16-iteration
  strict-monotonicity loop.
- Test-only hook `ubi_secure_test_get_volume_cached_counter()` exposes
  the per-volume cache for white-box tests.

## [0.80.0] - 2026-05-11

### Changed

- Plain UBI's three flow diagrams (Write / Read / Erase-Reclaim) in
  *Plain UBI Architecture* are now a single side-by-side SVG
  (`doc/img/plain_flows.svg`) instead of three stacked Mermaid blocks.
  Same information, one screenful, no client-side render.

## [0.79.0] - 2026-05-11

### Added

- A new Getting Started page, **Comparison: UBI vs LittleFS vs NVS vs
  ZMS**, places UBI side-by-side with the three main Zephyr-native
  storage subsystems and answers "which one do I pick?" in a single
  table plus a two-question decision shortcut. Linked from the
  Getting Started toctree and from the "When not to use UBI" section
  of *What is UBI?*.

### Changed

- Removed redundant `{contents}` blocks from the Cookbook, Error Codes
  and Kconfig Reference pages — Furo already renders a per-page
  outline in the right sidebar.

### Fixed

- The GitHub icon in the page footer is now rendered as inline SVG
  instead of an `<img src="_static/…">` reference, so it shows up on
  nested URLs (e.g. `architecture/secure_overview.html`) where the
  relative path previously resolved to a 404.

## [0.78.0] - 2026-05-11

### Changed

- Documentation site now uses the **Furo** theme instead of
  `sphinx_rtd_theme`. Furo ships with a working light / dark mode
  toggle, a copy-button on every code block, a sticky sidebar, and
  "Edit on GitHub" links generated automatically from the
  `source_repository` configuration. No content was touched and no
  URLs change.

## [0.77.0] - 2026-05-11

### Changed

- Plain UBI Architecture flow-diagram headings ("Read Flow", "Erase /
  Reclaim Flow") no longer carry a "(Mermaid)" suffix — readers do not
  need to know the rendering backend.
- Plain UBI Architecture prose now uses the canonical field names
  `volume_id` and `vid_sqnum` (previously `vol_id` and `sqnum`) when
  describing the VID header and crash-recovery rule, aligning the
  document with the C headers and the Glossary. Literal byte-layout
  tables and the in-RAM cache diagrams keep the original short field
  names since those are the actual symbols.

### Fixed

- Removed a stale placeholder from the Secure
  Architecture Overview "What's next" list — the referenced Cookbook
  recipes have been live since v0.71.0.

### Added

- A small Sphinx stylesheet (`_static/custom.css`) caps Mermaid
  diagrams at 720 px and centres them, so the three plain-architecture
  flow diagrams render at a consistent visual size regardless of how
  many nodes each one has.

## [0.76.0] - 2026-05-08

### Changed

- The Secure On-Flash Format Specification no longer relies on Mermaid
  for its three illustrations. The §6.2 key-hierarchy diagram now
  reuses the existing `key_hierarchy.svg` figure (the same one shown
  in the Secure Architecture Overview), and the §16 attach-time and
  runtime sequence diagrams have been redrawn as static SVGs
  (`secure_attach_sequence.svg`, `secure_runtime_sequence.svg`) using
  the same palette as the rest of the documentation. The diagrams
  now render identically on GitHub, on GitHub Pages, and in any
  offline / PDF rendering of the documentation, with no dependency
  on a Mermaid renderer.

## [0.75.0] - 2026-05-08

### Added

- A **social preview banner** (`doc/img/social_preview.svg` plus a
  rendered 1280×640 PNG) ships with the repository and is intended to
  be uploaded as the GitHub repository's social preview image.
- The README now includes an **Acknowledgments** section that credits
  the Linux UBI subsystem as the direct inspiration for the
  wear-leveling, dual-bank, and LEB-to-PEB mapping design, and frames
  Secure UBI as the author's own addition on top of that foundation —
  motivated by the absence of a Zephyr storage/volume-management layer
  with full on-disk authenticated encryption.

### Changed

- The README's release-status note has been promoted from a Sphinx
  admonition to a clearly visible blockquote at the top of the file,
  and the Documentation section now links to the new
  **Glossary** alongside the existing reference pages.

## [0.74.0] - 2026-05-08

### Added

- A single-page **Glossary** (`reference/glossary`) collects every
  UBI-specific term used across the documentation — PEB, LEB, EC, VID,
  EBA, `vid_sqnum`, `global_sqnum`, `volume_id`, dual-bank, reserved
  generation, freshness store, write-active key version, allowlist,
  hidden anchor, `KEY_RETIRABLE`, strict read-only, and more. The
  glossary is reachable from the index "Reference" card and from the
  Reference toctree.

### Changed

- Plain UBI Architecture now uses the precise field names `vid_sqnum`
  and `volume_id` where it previously said "sequence number" and
  `vol_id` in prose, so the document and the C headers agree on
  vocabulary.

## [0.73.0] - 2026-05-08

### Changed

- The Secure On-Flash Format Specification is now a tighter normative
  reference. Architectural rationale that previously appeared inside
  the spec body — the threat-model narrative, the security boundary,
  the key-state taxonomy, the lazy-vs-forced rekey discussion, and
  several "why this design is chosen" sidebars — has been removed
  from the spec and replaced with cross-links to the Secure
  Architecture Overview and the Secure UBI Workflow guide, which were
  already the canonical homes for that material. Readers looking for
  byte-level rules now reach them sooner; readers looking for the
  architectural reasoning still find it, in one place.
- The *Application contract* section in the Secure Architecture
  Overview is now a four-row map (PSA key provider, allowlist,
  freshness store, event callback) that points into the matching
  subsections of the Secure UBI Workflow guide. The detailed
  signatures and verdict semantics live in the workflow guide as the
  single source of truth, instead of being duplicated across both
  pages.

## [0.72.0] - 2026-05-08

### Added

- New **Plain UBI Workflow** guide — a developer-oriented walkthrough
  covering when to use plain UBI, the `ubi_device` lifecycle, volume
  management, the LEB read/write contract, garbage collection,
  error handling, and degraded read-only mode. Previously the only
  way to assemble this picture was to read the API reference, the
  Architecture page, and the Cookbook in parallel.
- New **Error Codes** reference — every value the public API can
  return is now listed in one table with the recommended
  application-level reaction, plus per-code notes for the
  non-obvious cases (`-ENOSPC` overloading, `-EROFS` self-healing,
  `-EBADMSG` as a security event).

### Fixed

- Quick Start example used `sizeof(data)` as the read length, which
  silently truncated the read buffer. Now reads `sizeof(buf)`.
- Cookbook § *Periodic garbage collection* clarified that the
  application's main translation unit must contain a matching
  `LOG_MODULE_REGISTER(app)` for the snippet's `LOG_MODULE_DECLARE`
  to link.
- Test Strategy coverage table label refreshed from `(v0.22.0)` to
  `(v0.71.0)`.

## [0.71.0] - 2026-05-08

### Documentation

- Documentation reorganised into five topic folders
  (`getting_started/`, `guide/`, `architecture/`, `reference/`,
  `project/`). Old URLs continue to work via redirects, so existing
  bookmarks and external links are preserved.
- New **Kconfig Reference** page lists every UBI Kconfig symbol
  with its type, default, range, dependencies, and a one-paragraph
  description. The *Configuration* guide now links to it instead of
  duplicating a less-detailed table.

### Changed

- The reported flash footprint of the UBI library is now ~9.5 KB
  plain and ~29 KB secure on Cortex-M33 (`-Os`), measured against
  the UBI archive only. PSA Crypto / mbedTLS is provided by the
  platform and is not counted, so the number reflects the library
  cost independently of the platform crypto stack.

### Fixed

- `key_hierarchy.svg`: the `K_volume_identifier[v]` label and the
  `K_leb` sub-caption no longer overflow their boxes.
- `onflash_layout.svg`: added the previously-missing zoom into a
  reserved (dual-bank metadata) PEB so the diagram now shows
  *both* the reserved PEB and the data PEB layouts.
- `onflash_format_spec.md`: chapters 19 (Secure volume lifecycle),
  20 (Secure recovery scenarios), and 21 (Runtime policy) now
  appear before *Appendix A*. Previously the appendix was sandwiched
  between §18 and §19, which made the table of contents confusing.

### Removed

- *Appendix B* and *Appendix C* of `onflash_format_spec.md`. Their
  content (roadmap items and a release checklist) is fully covered
  by `project/roadmap.md` and the ZTEST traceability tables in
  `project/test_strategy.md`.

## [0.70.0] - 2026-05-07

### Changed

- The Cookbook chapter has graduated from a stub list of intended
  recipes to six runnable, copy-paste-ready end-to-end recipes:
  bringing UBI up on STM32U5 (`b_u585i_iot02a`) with a 128 KiB
  partition; bringing UBI up on nRF5340 (`nrf5340dk/nrf5340/cpuapp`)
  with a 64 KiB partition; A/B firmware slots backed by two
  `STATIC` volumes plus a small `fw_meta` volume to flip the active
  marker; periodic garbage collection driven by a delayable
  `k_work` that calls `ubi_device_erase_peb()` and also serves the
  degraded read-only self-heal path; key rotation against PSA from
  version 1 to version 2, including the exact moment at which it is
  safe to call `psa_destroy_key()` (after `KEY_RETIRABLE`); and a
  freshness store implemented on top of Zephyr Settings, with both
  the attach-time `check_freshness` callback and the post-commit
  `sync_freshness` callback wired up. Each recipe spells out the
  pitfalls and the tuning knobs in addition to the code, so the
  developer reading it ends up with a working integration rather
  than a snippet that compiles but is wrong on the second power
  cycle.
- Three hero diagrams have been promoted from ASCII / Mermaid to
  hand-authored SVG and live under `doc/img/`. `stack.svg` shows
  the four-layer software stack (Application → UBI Public API →
  Zephyr `flash_area` / PSA Crypto → Physical Flash) and replaces
  both the README ASCII art and the small Mermaid graph in
  *What is UBI?*. `onflash_layout.svg` shows the partition as a
  strip of PEBs (reserved / mapped / free / dirty) plus a zoomed-in
  PEB with EC header at offset 0, VID header at offset 512, and
  the LEB data area, replacing the outer ASCII frame in *Plain
  Architecture* (the byte-level reserved-PEB ASCII stays — the SVG
  glosses over the volume-header table inside reserved PEBs).
  `key_hierarchy.svg` shows the Secure UBI key tree
  (`IKM[v]` →`HKDF-Extract` → `PRK[v]` → `HKDF-Expand` → five
  per-domain child keys, with the LEB key further bound to the
  durable `volume_id`) and replaces the Mermaid version in
  *Secure Overview*. SVGs are theme-neutral so they read on both
  the GitHub README (light/dark) and the Sphinx site.
- `README.md` has been slimmed to its v1.0.0 target shape: the
  ASCII stack box is gone in favour of `doc/img/stack.svg`, and the
  long Documentation table has collapsed into a short list of seven
  curated bullet links plus a one-line pointer to the published
  docs site. The Properties / When to Use / When NOT to Use / Quick
  Start / Project Quality / License / Contact sections are
  unchanged. Total length drops by roughly 30%.

## [0.69.0] - 2026-05-07

### Changed

- The Secure UBI documentation has been split along the
  audience boundary it always conflated. What used to be a
  single 3000-line `secure_architecture.md` page — half
  developer onboarding, half normative byte-level reference,
  half release checklist — is now three pages addressed to
  three different readers. The normative content (record
  layouts, AAD bindings, nonce rules, on-flash counter
  semantics, dual-bank reserved metadata, anchor witnesses,
  recovery scenarios, runtime policy) now lives in
  `onflash_format_spec.md` (renamed from `secure_architecture.md`
  via `git mv` so version history is preserved). The two
  developer-facing entry points are new: `secure_overview.md`
  is a five-page Architecture chapter that explains what
  Secure UBI does, the key hierarchy with one Mermaid diagram,
  the threat model as a table, the application contract
  (allowlist, RNG, freshness callback, event callback), and
  the four-state key lifecycle (soft rotation → live rewrite
  → media scrub → key retired) again with one Mermaid diagram.
  `secure_workflow.md` is a User Guide chapter that walks the
  application author through prerequisites, `crypto_cfg`
  fields, the exact callback contracts, lazy and forced key
  rotation as workflows, the event handler shape, and
  retirement.
- The three small secure spec satellites
  (`secure_volume_lifecycle.md`, `secure_recovery_notes.md`,
  `secure_runtime_policy.md`) have been folded into
  `onflash_format_spec.md` as chapters 19, 20, and 21
  respectively, so the format spec is now a single
  self-contained reference instead of a hub page that
  scatters readers across orphan satellites.
- The Secure UBI ZTEST traceability tables (Lifecycle step →
  ZTEST, Recovery scenario → ZTEST, Release checklist →
  ZTEST) have been moved out of the user-facing secure docs
  and into `test_strategy.md` under a new dedicated section.
  This restores user docs to "what the system does"
  (specification + workflow) and concentrates "how the system
  is verified" (ZTEST mapping) in the one place where test
  strategy is already discussed.
- `doc/conf.py` gains four redirects (`secure_architecture`,
  `secure_volume_lifecycle`, `secure_recovery_notes`,
  `secure_runtime_policy` → `onflash_format_spec.html`) so
  external bookmarks and search-engine results to any of the
  four old URLs continue to land on the live spec.
- `README.md` and `doc/plain_architecture.md` now point at
  `secure_overview.html` for the developer-facing entry and
  at `onflash_format_spec.html` for the normative reference,
  so newcomers no longer hit the 3000-line spec as their
  first encounter with Secure UBI.

## [0.68.0] - 2026-05-07

### Changed

- The Plain Architecture documentation has been slimmed of
  duplicate renderings and tooling material. The PEB lifecycle
  state machine is now rendered only as Mermaid — the duplicate
  ASCII version of the same diagram has been removed — and the
  per-file source-code map (which `.c` file holds which
  responsibility) has moved to a new orphan *Developer Notes*
  page intended for contributors rather than library users.
  The legacy *Getting Started* page has been split into a
  hands-on *Quick Start* under §1.2 of the new sidebar (build
  on `native_sim`, run the sample, write your first volume in
  about five minutes) and a build/test/coverage section folded
  into *Test Strategy* under §5.3, so that prerequisites,
  cross-compilation recipes, coverage instructions, the
  forensic scan, and the formatting check now live alongside
  the test categories they describe. Internal cross-references
  in *Contributing*, *Configuration*, and the *Plain UBI
  Workflow* stub have been repointed, and the README
  documentation table now links to *Quick Start* instead of
  *Getting Started*; `sphinx-reredirects` keeps the old URL
  alive by forwarding it to *Quick Start*. This is the third
  pass (PR 3 of 5) of the v1.0.0 documentation restructure.

## [0.67.0] - 2026-05-07

### Changed

- The legacy `Overview` and `Introduction` documentation pages have
  been merged into a single canonical *What is UBI?* page and a
  companion *Concepts at a Glance* page. New readers now hit the
  same elevator pitch and decision criteria once instead of three
  times across overlapping pages. The runtime model (PEB, LEB, EC,
  VID, EBA) and the six-step lifecycle now live on a dedicated
  concepts page that the architecture and API references link
  back to. The resource-usage profile previously kept on the
  *Introduction* page has moved into the *Plain Architecture*
  guide alongside the existing memory-usage section, so flash and
  static-RAM figures sit next to the in-RAM data structures they
  describe. The README documentation table, all internal Markdown
  cross-references, and the `contributing` guide have been updated
  to point at the new pages, and `sphinx-reredirects` keeps the
  legacy URLs alive by forwarding them to *What is UBI?*. This is
  the second pass (PR 2 of 5) of the v1.0.0 documentation
  restructure.

## [0.66.0] - 2026-05-07

### Changed

- The Sphinx documentation sidebar has been reorganised into five
  Diátaxis-aligned sections (Getting Started, User Guide,
  Architecture, Reference, Project) with a four-card landing page
  and a v1.0.0 status banner. This is the scaffolding pass of the
  v1.0.0 documentation restructure (PR 1 of 5): no page content
  has been rewritten. Ten stub pages have been added as targets
  for content merges in subsequent PRs, and seven legacy pages
  (Overview, Introduction, Getting Started, Secure Architecture
  and the three secure operational notes) are now orphaned with
  deprecation banners pointing to their new homes — their URLs
  remain reachable while the content migration is in progress.
  The positioning piece *Why UBI matters on Zephyr* has moved out
  of the published sidebar into `doc/positioning/`, and its old
  URL now redirects to the GitHub source via `sphinx-reredirects`,
  which is added as a new documentation build dependency.

## [0.65.0] - 2026-05-07

### Changed

- The in-tree test suites are now organised to match the project
  coding style. Lifecycle hooks, helpers and ZTEST cases are grouped
  under canonical section banners with documented setup, teardown
  and per-case scenarios. This change is internal: behaviour of the
  library, public API and on-flash format are unaffected. Downstream
  users who run the bundled tests will see clearer log headings and
  consistent file structure across every suite.

## [0.64.0] - 2026-05-07

### Changed

- The secure backend now treats misuse of internal vtable entry
  points the same way as the public dispatcher: passing `NULL`
  arguments to `ubi_secure_anchor_create()` or
  `ubi_secure_device_init()` returns `-EINVAL` with a diagnostic
  log instead of tripping an assertion. Production builds without
  asserts no longer experience undefined behaviour on a `NULL`
  misuse — the call fails cleanly.
- The secure onboarding sample now emits its output through the
  Zephyr logging subsystem (`LOG_INF` / `LOG_ERR`) instead of
  `printk`. Sample messages can therefore be filtered, redirected
  and silenced via the same log-level controls as the library
  itself, and every failure path includes the underlying return
  code.

## [0.63.0] - 2026-05-07

### Changed

- Misuse of the public API now surfaces as an actionable diagnostic
  instead of a silent `-EINVAL`. Calling `ubi_device_*`,
  `ubi_volume_*` or `ubi_leb_*` with `NULL` arguments or out-of-range
  identifiers logs which argument was invalid (including pointer
  values for multi-argument checks), so integration bugs are caught
  at the first failing call. Return codes are unchanged.
- The plain sample now emits its output through the Zephyr logging
  subsystem (`LOG_INF` / `LOG_ERR`) instead of `printk`. Sample
  messages can therefore be filtered, redirected and silenced via
  the same log-level controls as the library itself, and every
  failure path includes the underlying return code.

## [0.62.0] - 2026-05-07

### Added

- Cross-reference tables in the secure documentation so users can
  jump from a documented behaviour straight to the regression test
  that locks it. `secure_recovery_notes.md` now ends with a
  "Scenario → ZTEST coverage" table covering all nine power-cut /
  anchor-migration / dual-bank scenarios. `secure_volume_lifecycle.md`
  ends with a "Lifecycle step → code & ZTEST coverage" table mapping
  each of the seven volume lifecycle steps (create / grow / shrink /
  remove / unmap / erase & reclaim / reboot recovery) onto the
  implementing function in `lib/src/secure/` and the secure ZTESTs
  that exercise it. `secure_architecture.md` gains an Appendix D
  ("Release checklist → ZTEST coverage") that mirrors Appendix C
  bullet-by-bullet, pointing each release-checklist item at the
  concrete tests under `tests/src/secure/`. Two review-only bullets
  (scratch zeroization, `device_revision` ordering) are tagged as
  such with a short rationale.

## [0.61.0] - 2026-05-07

### Added

- A ready-to-run onboarding sample for the secure (AEAD-backed)
  UBI backend, so users have a single, copy-pasteable starting
  point for bringing up an encrypted volume. Build it with
  `west build -p -b <board> sample/ -- -DOVERLAY_CONFIG=boards/secure.conf`;
  CMake automatically selects the secure entry point when
  `CONFIG_UBI_CRYPTO=y`. The sample shows how to initialise PSA
  Crypto, import a root key, wire the four secure callbacks
  (`get_key_id`, `check_freshness`, `sync_freshness`, `event_cb`)
  and run a minimal single-version key policy end-to-end.
- The secure sample is now compile-verified on every PR for the
  supported Cortex-M targets (`b_u585i_iot02a` and
  `nrf5340dk/nrf5340/cpuapp`, both `static` and `heap` backends),
  so regressions in the secure onboarding flow are caught in CI
  instead of by downstream users.

### Fixed

- The library no longer fails to build for downstream consumers
  that link it without `CONFIG_UBI_TEST_API_ENABLE=y` (including
  the new sample). The `struct ubi_device` size build-time
  assertions previously hard-coded the in-tree test layout; they
  now account for the test-only field and keep locking both the
  production and test layouts on every supported architecture.

## [0.60.0] - 2026-05-07

### Added

- Forced-rekey end-to-end regression test
  (`ubi_secure_runtime_policy.test_forced_rekey_with_stale_objects`)
  covering the scenario from `secure_architecture.md` Appendix B §3:
  after the operator raises `requested_write_key_version` while
  stale free, dirty and mapped data PEBs from the previous key
  version still exist on flash, the attach path eagerly upgrades
  reserved metadata to the new key version, every previously
  committed payload remains readable across the rotation, new
  writes commit under the new key version, and the
  `KEY_RETIRABLE` event for the retired key version is withheld
  until the last stale object has been recycled and then fires
  exactly for the retired key version. This closes the final
  outstanding security-relevant audit gap.

## [0.59.0] - 2026-05-07

### Added

- Chunked-write 48-bit AEAD counter overflow regression test
  (`ubi_secure_chunked.test_chunked_write_overflow_rejected`). A
  multi-chunk write whose projected per-LEB write counter would
  exceed `UBI_SECURE_COUNTER_MAX` is now proven to fail closed before
  any flash mutation: the call returns `-EOVERFLOW`, exactly one
  `KEY_ROTATE_NOW` event is emitted with `usage_pct=100`, no free
  PEB is consumed, and the previously committed payload survives.
  This closes the remaining safety-net gap identified by the audit
  for the chunked write path.

### Changed

- New test-only API
  `ubi_secure_test_set_leb_write_counter_floor()` /
  `ubi_secure_test_get_leb_write_counter_floor()` (compiled in only
  when `CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION=y`). It clamps the
  per-LEB write counter recovered from flash up to a configurable
  floor so the chunked-write overflow guard can be exercised
  end-to-end without performing 2^48 real chunk encryptions.

## [0.58.0] - 2026-05-06

### Fixed

- Restored the `native_sim` chunked test build. The chunked board
  config inherited an outdated baseline and no longer matched what
  the secure test suites depend on; this prevented the chunked
  configuration from being verified at all.
- Removed a cascading-failure pattern in the secure test runner:
  when a single test aborted before completing teardown, every
  subsequent test in the same binary observed exhausted internal
  pools and reported a misleading allocation failure. The shared
  test-only reset path now fully restores the allocator state, so
  a leak in one test no longer poisons the rest of the run.
- Hardened the chunked tamper test against the flash simulator's
  NAND-style write semantics. The previous bit-flip strategy was
  silently a no-op on roughly half of the runs, depending on the
  random ciphertext byte, and is now replaced by a deterministic
  destructive write with a read-back guard.
- Aligned the coverage-secure CI configuration with the runtime
  contract assumed by the coexistence test (two concurrent device
  handles). The CI build was previously inheriting the single-handle
  default, so the coexistence test failed only on the
  `coverage-secure` job.
- Tightened the host-side flash forensic scanner
  (`scripts/scan_flash.py`) so that it no longer flags ciphertext as
  a leak. The previous 8-character printable-ASCII threshold was
  statistically guaranteed to false-positive on encrypted regions
  once a second secure partition was added to the test image; the
  scanner now uses a 20-character threshold and ignores the secure
  prefix magic, while explicit binary patterns (test arrays, root
  key, volume names) still detect real plaintext leaks.

## [0.57.0] - 2026-05-06

### Added

- Plain + secure backend coexistence regression test
  (`ubi_secure_coexistence` suite): a plain UBI device on
  `ubi_partition` and a secure UBI device on a second partition
  `ubi_partition_2` operate in parallel without state bleed-through,
  survive a deinit/reattach cycle, and the partition guard still
  rejects a second attach to either partition with `-EBUSY`.

### Changed

- Test build raises `CONFIG_UBI_MAX_NR_OF_DEVICES` to `2` for the
  `native_sim` secure config so the coexistence suite can hold two
  device handles concurrently. Production defaults are unaffected.

## [0.56.0] - 2026-05-06

### Added

- Replay-to-other-location regression tests (`ubi_secure_replay` suite):
  forging an authentic EC, VID, or LEB record into a different physical
  PEB is rejected by the parent-child AAD binding (`peb_index` in AAD).
  EC and VID replay marks the destination PEB as bad on reattach; LEB
  replay raises `AUTH_FAILURE` on the next read of the affected mapping.

## [0.55.0] - 2026-05-06

### Added

- Plain dispatcher test verifying `ubi_device_init()` returns `-ENOTSUP`
  when a non-NULL `crypto_cfg` is passed but `CONFIG_UBI_CRYPTO=n`.

### Changed

- `ubi_device_init()` now rejects, with `-EINVAL`, a `crypto_cfg` whose
  `allowed_key_versions` contains duplicate entries.
- Secure attach now rejects, with `-EINVAL`, a reattach whose
  `requested_write_key_version` is below the on-flash
  `write_active_key_version` (key versions must be monotonically
  non-decreasing; protects against accidental downgrade and uint8_t
  wrap-around).

## [0.54.0] - 2026-05-06

### Added

- Public API `ubi_secure_get_write_active_key_version()` that returns the
  authenticated write-active key version of a secure UBI device.  Returns
  `-EINVAL` on NULL arguments and `-ENOTSUP` on a plain-mode device.
- Init-time reserved-generation fit guard: secure init rejects
  geometries where one reserved generation
  (`UBI_SECURE_DEV_HDR_SIZE + CONFIG_UBI_MAX_NR_OF_VOLUMES * UBI_SECURE_VOL_HDR_SIZE`)
  cannot fit inside one reserved PEB.
- Five ZTESTs in `ubi_secure_api` and `ubi_secure_forensic`:
  `test_get_write_active_kv_null_args`,
  `test_get_write_active_kv_plain_mode`,
  `test_get_write_active_kv_after_format_and_rotation`,
  `test_reserved_generation_fit_guard_rejects_small_eb`,
  `test_leb_tail_padding_uses_erased_value`.

### Changed

- LEB write tail padding now uses the flash erased value (typically `0xFF`)
  instead of `0x00`, so the unused tail of the final write block matches
  what an erase would leave behind.  Applies to both single-tag and
  chunked write paths.
- `secure_architecture.md` §7.10 expanded with an implementation note
  stating that the secure read path deliberately does not re-verify
  inner CRC fields after a successful AEAD verification (the CCM tag
  already covers full record integrity).

## [0.53.0] - 2026-05-06

### Changed

- Eager reserved-PEB upgrade on key-version rotation now resets the
  on-flash and in-RAM VID-domain counter floor to 0.  Reattaching
  with the same key version still preserves the monotonic floor.
- Reserved-PEB key-version refcount now also counts secure volume
  headers (one per volume on every reserved PEB), and reserved
  metadata commits release the old (kv, vol_count) contribution only
  after the new one has been added.  Documentation in
  `secure_runtime_policy.md` updated to match `secure_architecture.md`.

### Added

- Four ZTESTs in `ubi_secure_runtime_policy`:
  `test_vid_floor_resets_on_rotation`,
  `test_vid_floor_persists_within_same_kv`,
  `test_vid_floor_reset_writes_use_low_counters`,
  `test_reserved_refcount_no_spurious_key_retirable`.

## [0.52.0] - 2026-05-05

### Changed

- Secure read paths reject records with an unknown `prefix32.wrapper_version`
  (`-EBADMSG`) right after the magic check, before any key derivation.
  Applied at all six deserialize sites: EC, VID, LEB single-tag, LEB
  chunked, reserved-PEB device header, reserved-PEB volume header.
- Single-tag LEB IO path enforces the AES-128-CCM payload limit at
  runtime: with q = 2 the CCM length field is 2 bytes, so any payload
  of 65536 bytes or more is unrepresentable.
  `ubi_secure_leb_data_write` rejects `len > 65535` with `-EFBIG`;
  `ubi_secure_leb_data_read` rejects `vid_hdr->data_size > 65535` with
  `-EBADMSG`; both checks fire before any key derivation or flash IO.
  In non-chunked builds, device init rejects geometries whose
  `leb_size > 65535` with `-EINVAL`.

### Added

- New constant `UBI_SECURE_LEB_SINGLE_TAG_MAX_PAYLOAD` (= 65535).
- Seven defensive ZTESTs in `ubi_secure_defensive`:
  `test_init_dev_hdr_bad_wrapper_version`,
  `test_init_vol_hdr_bad_wrapper_version`,
  `test_io_ec_hdr_read_bad_wrapper_version`,
  `test_io_vid_hdr_read_bad_wrapper_version`,
  `test_io_leb_data_read_bad_wrapper_version`,
  `test_io_leb_data_write_payload_exceeds_ccm_limit`,
  `test_io_leb_data_read_data_size_exceeds_ccm_limit`.

## [0.51.0] - 2026-05-05

### Added

- Secure backend now enforces a per-domain write-budget for all four
  metadata domains (device header, volume header, erase counter, volume
  identifier) under the active write key version, alongside the existing
  per-LEB data budget.  Reaching `CONFIG_UBI_CRYPTO_ROTATE_SOON_PCT` of
  any budget emits `KEY_ROTATE_SOON` while the operation still completes;
  reaching `CONFIG_UBI_CRYPTO_ROTATE_NOW_PCT` emits `KEY_ROTATE_NOW`,
  rejects the operation with `-ENOSPC` and transitions the device to
  read-only.  Subsequent writes, erases and volume mutations return
  `-EROFS` until the device is reattached with a new
  `requested_write_key_version`; reads remain available throughout.
- New `ubi_secure_budget_pre()` / `ubi_secure_budget_post()` API takes a
  `ubi_secure_domain` so callers no longer have to pick between
  metadata-only and LEB-only variants.

## [0.50.0] - 2026-05-04

### Changed

- Plain backend: public API functions now return `-EINVAL` with a log message on NULL arguments instead of triggering a kernel assert.
- Plain backend: all error paths now emit `LOG_ERR` for diagnostics.
- Plain backend: `reclaim_peb_to_dirty()` no longer returns an error code (always succeeded).
- Plain backend: `leb_write()` no longer acquires the device mutex — the public `ubi_leb_write()` wrapper handles locking.
- Plain backend: reserved-PEB overwrite fixed a dual-bank safety issue where a replacement PEB could be re-erased within the same commit.

## [0.49.0] - 2026-05-04

### Changed

- Renamed `struct ubi_mtd` → `struct ubi_flash_desc` and `.mtd` → `.flash` field across the codebase.
- Reordered `struct ubi_device` fields for better logical grouping.
- Consolidated `CONFIG_UBI_CRYPTO_MAX_ALLOWLIST_LEN` into `CONFIG_UBI_CRYPTO_MAX_KEY_VERSIONS`.
- Renamed `CONFIG_UBI_MEM_STATS` → `CONFIG_UBI_TEST_MEM_STATS`.
- Added `depends on MBEDTLS_ENTROPY_POLL_ZEPHYR` to `UBI_CRYPTO`.
- Removed dead code, improved doxygen, and standardized comment style across all files.

## [0.48.0] - 2026-04-17

### Changed

- Secure crypto: central allowlist check in `derive_domain_key()` and `derive_leb_key()` — all domains and both read/write paths validated before key derivation.
- Secure backend: monotonic AEAD counters for EC and device-header domains — recovered from flash during init, incremented per write.
- Secure backend: VID counter overflow check (`COUNTER_MAX`) with `KEY_ROTATE_NOW` event.
- Secure backend: overflow guards in `ec_hdr_write`, `vid_hdr_write`, and `res_peb_commit`.
- Secure backend: eager reserved-PEB key upgrade during attach — when `requested_write_key_version` differs from flash, `res_peb_commit` rewrites device and volume headers under the new key immediately, with graceful fallback if the key is not yet provisioned.
- Secure backend: reserved-PEB refcount tracking via `reserved_key_version` field — `KEY_RETIRABLE` blocked while reserved PEBs still depend on old key version.

### Added

- Test: VID counter floor survives `remove→create→reboot` sequence (2 cycles).
- Test: refcount E2E lifecycle with key rotation (`kv=1→kv=2`), volume ops, unmap, resize, erase cycles, and `KEY_RETIRABLE` verification.
- Tests: counter overflow boundary tests for EC, VID, and reserved-PEB AEAD counters.
- Tests: allowlist rejection tests for `derive_domain_key` and `derive_leb_key`.

## [0.47.0] - 2026-04-17

### Changed

- Plain backend: renamed 7 `.c` and 2 `.h` files to `ubi_plain_*` prefix; updated all includes and CMake.
- Secure backend: replaced `ubi_secure_zeroize` volatile-loop with `mbedtls_platform_zeroize`.
- Secure backend: added `LOG_ERR` to silent error returns in volume, LEB, runtime, and crypto modules.
- Secure backend: removed `§` references from comments; fixed `ubi_secure_anchor_create` section label.
- Tests: normalized section divider comments to 100-char centered format.

## [0.46.0] - 2026-04-17

### Added

- Secure error-handling test suite (74 tests): device init/deinit, volume CRUD, LEB I/O, resize, and edge-case contract enforcement mirrored from the plain backend.
- Secure fault-injection test suite (2 tests): transactional safety under allocation failures and overwrite faults.
- Secure mutation-gate test suite (1 test): write-shutdown gate enforcement via `ubi_test_set_write_shutdown()`.
- Secure vol-id-watermark test suite (3 tests): persistent volume-ID high-watermark across remove, reinit, and slot re-indexing.

## [0.45.0] - 2026-04-16

### Changed

- Documentation aligned with secure backend implementation: updated flash footprint, struct sizes, test counts, coverage numbers, and Kconfig reference across all docs.
- Merged README Design Trade-offs into Key Properties; added authenticated encryption and dynamic resize entries.
- Removed completed crypto layer from roadmap (only shell commands remain as planned).

### Fixed

- CI `format-check`: clang-format version-dependent goto label formatting resolved with off/on guards.
- CI `cross-build` nRF5340 secure: added test random generator for builds without BLE IPC.
- CI `cross-build` STM32U5 secure: `ubi_device` size BUILD_ASSERT guarded for ARM vs POSIX `k_mutex` layout difference.

## [0.44.0] - 2026-04-16

### Added

- Forensic scan test suite (5 tests): verifies no plaintext data, volume names, or key material appear on flash after secure writes.
- Host-side Python forensic scanner and test docblock checker scripts.
- CI jobs: `format-check`, `forensic-scan`, cross-build secure dimension for STM32U5 and nRF5340.
- Init-time anchor re-creation: orphaned volumes automatically get a new hidden anchor at attach.

### Changed

- Test and coverage scripts accept mode parameter (plain/secure/chunked).

## [0.43.0] - 2026-04-16

### Added

- Chunked secure LEB mode: LEB records split into independently authenticated chunks for partial-read support. Configurable chunk size (256-65536 B).
- Chunked partial-read authentication: only touched chunks are verified, reducing read latency and RAM.
- Chunked geometry validation at init and budget accounting for chunked writes.
- Secure recovery test suite (8 tests): interrupted writes, anchor rewrites, reserved PEB commit faults, generation replay rejection.
- Flash write fault injection wired into the secure backend.

## [0.42.0] - 2026-04-15

### Added

- Sticky crypto read-only mode: event callback can escalate to device-wide write block (`-EROFS`); reads remain functional.
- Security event infrastructure: 10 event types (AUTH_FAILURE, FORMAT_VIOLATION, KEY_ROTATE_SOON/NOW, KEY_RETIRABLE, RNG_FAILURE, etc.) delivered through application callback.
- Freshness sync after every commit-visible mutation with configurable cadence.
- Key-version PEB refcount tracking with KEY_RETIRABLE signalling when a key version is fully erased.
- LEB usage budget tracking with soft/hard thresholds and pre-write rejection.
- Read-path key-version allowlist enforcement.
- Sensitive buffer zeroization (compiler-safe volatile memset).
- Runtime policy test suite (15 tests): read-only transitions, event escalation, budget thresholds, key retirement, allowlist rejection, mixed-key rotation.

## [0.41.0] - 2026-04-15

### Added

- Hidden per-volume anchor PEBs: each secure volume reserves one internal PEB preserving monotonic counter state across unmap, shrink, and erase.
- VID-domain counter floor: monotonic VID counter persisted in secure device header, reconstructed at attach.
- Last-writable-witness check: erase path rewrites anchor before erasing the last carrier of a volume's counter floor.
- Emergency free-PEB reserve: write path reclaims a dirty PEB before consuming the last free PEB.

### Changed

- Volume create rolls back on anchor allocation failure.
- Tests updated for anchor PEB overhead and wear-leveling participation.
- New docs: secure volume lifecycle and secure recovery scenarios.

## [0.40.0] - 2026-04-15

### Added

- Complete secure data-PEB runtime: volume create/resize/remove, LEB write/read/map/unmap, erase — all with authenticated encryption.
- Data-PEB scan pipeline: classifies PEBs into free/dirty/bad pools with full AEAD verification.
- Secure parity test suites (26 tests across 7 suites): functional parity with plain backend on encrypted flash.
- Flash geometry overlays for nRF5340 (4 KB erase) and STM32U5 (8 KB erase) on native_sim.
- Multi-geometry CI: tests run against three flash geometries (default, nRF5340, STM32U5).

### Fixed

- LEB write/nonce counters recovered from existing VID header (were hardcoded to 0).
- Write-block alignment padding for secure ciphertext+tag buffer.
- Missing bad-block torture and degraded-mode recovery in secure runtime.
- Geometry validation for erase/write block alignment.

## [0.39.0] - 2026-04-14

### Added

- Secure data-PEB I/O: authenticated read/write for EC headers, VID headers, and LEB data in single-tag mode.
- Domain-separated AAD serialization for all record types.

### Changed

- Key derivation centralized; parent authentication passed via typed context structs.

## [0.38.0] - 2026-04-15

### Added

- Secure reserved-PEB attach path: format-on-blank, attach-to-existing, mode mismatch detection.
- PSA Crypto integration: HKDF-SHA-256 key derivation, AES-128-CCM AEAD, salt generation.
- Encrypted dual-bank reserved PEB commit and authentication.
- Secure attach test suite (8 tests): format, re-attach, mode mismatch, freshness rejection, callback validation.

## [0.37.0] - 2026-04-14

### Added

- Secure public API types: crypto config, event types (tagged union), freshness descriptor, policy struct, and callback typedefs.
- Secure Kconfig surface: master enable, budget limits, rotation thresholds, chunked mode, PEB cache, freshness sync, strict RO policies.
- Crypto fault injection hooks (7 stages) for integration tests.
- Secure test profiles and board configs with Mbed TLS PSA.

## [0.36.0] - 2026-04-14

### Changed

- Full backend ops dispatch: all public API functions route through a vtable (11 ops). Plain backend functions renamed to `ubi_plain_*`.
- Internal headers decoupled from plain-specific includes, making them backend-agnostic.

## [0.35.0] - 2026-04-14

### Changed

- Unified init API: `ubi_device_init(mtd, crypto_cfg, &ubi)`. Passing `NULL` selects plain; non-NULL selects secure. Runtime backend dispatch via ops vtable.

## [0.34.0] - 2026-04-13

### Changed

- Secure architecture spec: per-device runtime mode selection (not per-build), tagged-union events, `check_freshness` confirmed as attach-time only.
- Repository layout split into `common/`, `plain/`, `secure/` namespaces for library and test sources.
- Format script made recursive with `--check` mode for CI.

## [0.33.0] - 2026-04-11

### Changed

- `design_proposal_crypto.md` promoted to `secure_architecture.md` as a first-class architecture document.
- `architecture.md` renamed to `plain_architecture.md`.
- All doc navigation updated for the new names.

## [0.32.0] - 2026-04-11

### Changed

- Secure architecture spec rewrite (v6): hidden per-volume anchor PEBs, secure device header with crypto metadata, full counter continuity framework, renamed child keys to domain names, simplified chunked mode (no per-chunk subkeys), expanded write-budget enforcement, precise AAD byte layouts for all record types, tagged-union events with verdicts.

## [0.31.0] - 2026-04-10

### Changed

- Secure architecture spec rewrite (v5): mode detection rules, normative KDF labels, single-tag CCM payload limit, zero-length LEB encoding, tail-padding rules, parent EC key_version in VID and LEB AAD, release checklist (Appendix C).

## [0.30.0] - 2026-04-09

### Added

- "Why UBI for Zephyr" positioning document: gap analysis, comparison with FCB/NVS/ZMS/LittleFS.

### Changed

- Secure architecture spec rewrite (v4): plain-core baseline assumptions, application boundary clarification, expanded key derivation and freshness-sync.

## [0.29.0] - 2026-04-09

### Added

- Persistent vol_id high-watermark: volume IDs are never reused across the device lifetime. Overflow returns `-ENOSPC`.
- Test suite (4 tests): reuse prevention, cross-reboot persistence, slot stability, overflow.

### Changed

- Volume matching by `vol_id` instead of positional index; re-index loop eliminated.

## [0.28.0] - 2026-04-09

### Added

- Central mutation gate: per-device read-only flag checked before every public mutator. Three mutation classes (reserved metadata, data path, maintenance).
- Runtime degradation detection and self-healing via reserved PEB bank recovery.
- Test-only write-shutdown API.
- Test suite (5 tests): write-shutdown, degradation, transparent recovery, bank recovery.

## [0.27.0] - 2026-04-10

### Changed

- Data PEB commit order changed to EC -> DATA -> VID. VID header is now the sole commit-visible record.
- Init scan distinguishes free PEBs from uncommitted writes by probing the data area.

### Fixed

- Uncommitted writes were misclassified as free under the new write order.

## [0.26.0] - 2026-04-09

### Changed

- Erased-state detection uses hardware-reported erase value instead of hardcoded `0xFF`.

## [0.25.0] - 2026-04-08

### Changed

- Secure architecture spec rewrite (v3): init classification, write/read/erase paths, key lifecycle with refcount retirement, events and policy, cost model, illustrative API (Appendix A).

## [0.24.0] - 2026-04-03

### Added

- Secure architecture design proposal (v2): AES-128-CCM for all on-flash structures, ESSIV nonces, per-domain key derivation, anti-rollback, crash-safe key rotation, external AAD callback.

## [0.23.0] - 2026-04-03

### Added

- nRF5340 DK board support (64 KB UBI partition, 4 KB erase blocks).

### Changed

- CI split into parallel jobs: native-tests, cross-build, coverage.
- Concurrency control, least-privilege permissions, path filters, SHA-pinned actions.

## [0.22.0] - 2026-04-02

### Added

- Flash I/O fault injection: controllable write and erase failures.
- Test suite `ubi_io_faults` (24 tests): flash I/O and malloc fault sweeps.
- Test suite `ubi_init_errors` (33 tests): geometry validation, partition guard, format/header corruption at init.
- 34 new error-handling tests and 6 new recovery tests.
- Long-term EC counter equality test (500 cycles, max deviation <= 2).

## [0.21.1] - 2026-04-02

### Fixed

- Sphinx `-W` flag: warnings now fail the doc build.
- Breathe + Doxygen 1.9.8 compatibility: switched to `doxygengroup` directives.

## [0.21.0] - 2026-04-02

### Added

- Static memory backend (default): all allocations via `k_mem_slab` pools instead of global heap.
- Memory abstraction layer with Kconfig-selectable backend (static/heap).
- Init-time validation: static backend verifies flash geometry fits configured pools.
- Dual-backend CI runs.

### Changed

- PEB tracking items share a union for in-place retyping during state transitions.
- Fault injection routed through memory abstraction layer.

## [0.20.1] - 2026-04-02

### Changed

- Removed read-write lock from roadmap (per-device mutex is sufficient).
- Renamed "User-space tools" to "Shell commands" in roadmap.

### Fixed

- Test API functions visible in Doxygen output.

## [0.20.0] - 2026-04-01

### Added

- Single-handle-per-partition guard: prevents two device handles for the same flash partition.
- Concurrency test suite (5 tests): multi-threaded readers/writers, deinit quiescence, double-init guard.

### Fixed

- `ubi_device_deinit()` acquires mutex before teardown, preventing races with in-flight operations.

## [0.19.0] - 2026-04-01

### Added

- Volume config validation, device/volume header semantic checks.
- Invariant checker API for tests.
- Allocation fault injection via Kconfig.
- Shared test fixtures and raw flash write helpers.

### Changed

- Transactional `ubi_volume_create()` and shrink: RAM state consistent with flash on failure.
- Copy-on-write `leb_write()`: old mapping preserved until new PEB is fully written.
- `ubi_leb_unmap()` is idempotent; `ubi_leb_map()` is a no-op when already mapped.

### Fixed

- PEB tracking loss on allocation failure during bad-block handling.

## [0.18.0] - 2026-04-01

### Added

- Overview doc with mental model, six-step lifecycle, and Mermaid diagrams.
- Architecture guide expanded: core invariants, Mermaid flowcharts, degraded-mode table.

### Changed

- README rewritten as landing page with stack diagram, key properties, quality metrics.
- All doc pages restructured with "what this page covers" framing.

## [0.17.1] - 2026-03-31

### Fixed

- Doc source file table and test counts aligned with Phase 3 file splits.
- Sample app expanded to demonstrate full lifecycle.

### Changed

- Removed per-file Doxygen version/date tags; replaced Yoda conditions with idiomatic style.

## [0.17.0] - 2026-03-31

### Changed

- Split `ubi_core.c` into init and runtime modules; split `ubi_io.c` into metadata and data modules. No functional changes.

## [0.16.0] - 2026-03-31

### Added

- `read_only_degraded` exposed in device info struct.
- Cached geometry in device struct (no flash I/O for `get_info()`).
- Thread-safety notes and precise `\retval` docs on all public functions.

### Changed

- `allocated_peb_count` renamed to `reserved_peb_count`.
- `-EROFS` propagated from degraded reserved PEB scan through init and mutators.

## [0.15.0] - 2026-03-31

### Added

- Volume name validation helpers (bounded, NUL-safe).
- Semantic validation of on-flash device headers.
- Flash geometry validation at init.

### Fixed

- Eliminated `strlen()` on raw flash fields (memory safety).
- Sequence number monotonicity: `global_sqnum` set to `max + 1` after scan.
- Volume headers always read from highest-revision reserved PEB.
- Reclaim error paths no longer leak PEBs.

## [0.14.0] - 2026-03-30

### Changed

- Reserved PEB module renamed with `ubi_flash_res_peb_*` prefix.
- Volume module deduplicated with shared helpers.

## [0.13.0] - 2026-03-30

### Added

- Bad block torture test with configurable cycles and per-PEB erase attempts.
- Runtime average erase counter tracking.

### Fixed

- Use-after-free in `leb_write()` write-fail path.
- Bad-block paths now update erase counter averages.

## [0.12.0] - 2026-03-30

### Added

- Configurable write retry count for data PEBs.

### Fixed

- Write failure in `leb_write()` now properly marks PEB as bad.

## [0.11.0] - 2026-03-27

### Added

- Initial crypto layer design proposal (AES-128-CCM via PSA Crypto API).

## [0.10.0] - 2026-03-27

### Added

- Reserved PEB management extracted into dedicated module.
- Configurable reserved PEB count (2-4) for cold spare support.
- 12 new tests (recovery, data_size boundary).

## [0.9.0] - 2026-03-26

### Changed

- Monolithic source restructured into core, volume, LEB, I/O, and cache modules.
- Init scan decomposed into 5 helpers.
- Overhauled Doxygen and Kconfig.

### Fixed

- 11 bugs from code quality audit.
- Memory leak in duplicate-LEB resolution.

## [0.8.0] - 2026-03-25

### Added

- Sphinx documentation with Read the Docs theme, deployed to GitHub Pages.
- Doxygen + Breathe auto-generated API reference.
- Architecture guide, getting started, configuration reference, contributing guide.

## [0.7.0] - 2026-03-25

### Added

- native_sim board support.
- Test suites: error handling, boundary, recovery, stress — 87 tests total.
- GitHub Actions CI with build, test, and Codecov.
- Coverage infrastructure and CI scripts.
- Test strategy documentation.

### Fixed

- `west.yml`: `cmsis_6` renamed to `cmsis` for Zephyr v4.0.0.

## [0.6.0] - 2026-03-24

### Added

- Architecture guide with ASCII diagrams (on-flash layout, PEB lifecycle, init flow, wear-leveling).
- Development roadmap and contributor guide.

### Fixed

- EBA table corruption during init when resolving sequence number conflicts.

## [0.5.0] - 2025-09-25

### Added

- Mutex-based synchronization for thread-safe operations.

## [0.4.0] - 2025-09-24

### Added

- Sample application for STM32U5.

### Changed

- Optimized flash I/O and improved logging.

## [0.3.0] - 2025-09-21

### Added

- `.clang-format` configuration.

### Changed

- Migrated from low-level flash APIs to Zephyr Flash Map (Flash Area API).

## [0.2.0] - 2025-09-10

### Added

- Volume support with static and dynamic types.
- Runtime resizing, write block alignment, partial dual-bank support.
- Hardware tests on STM32U5.

## [0.1.0] - 2025-07-25

### Added

- Initial release: device init/deinit, LEB I/O (map, unmap, read, write), PEB statistics.
- Hardware integration tests and sample application for STM32U5.
- Environment setup documentation.

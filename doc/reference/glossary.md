# Glossary

A single-page lookup for every UBI-specific term used in this
documentation. Where a concept also has a longer treatment, the entry
links to it.

```{glossary}
:sorted:

PEB
    **Physical Erase Block.** A hardware erase unit on the flash chip
    (typically 4 KB on internal flash, 64 KB on NAND). The smallest
    unit the flash controller can erase. UBI manages a partition as a
    sequence of PEBs.

LEB
    **Logical Erase Block.** A volume-relative virtual block exposed
    to the application. UBI maps each LEB to one PEB at a time via
    the EBA table. From the application's point of view, LEBs hide
    wear-leveling, bad blocks, and crash-time PEB swaps.

EC
    **Erase Counter.** A 32-bit value stored in every PEB's EC
    header, incremented on every successful erase. Drives
    wear-leveling: the next free PEB chosen for a write is always the
    one with the lowest EC.

EC header
    The 16-byte header at offset 0 of every data PEB. Contains
    `magic`, `version`, the erase counter, and `hdr_crc`. Authenticated
    by AEAD in Secure UBI.

VID header
    **Volume Identifier header.** The 32-byte header at offset 0x010
    of every mapped data PEB. Contains the `volume_id`, `lnum`, and
    `vid_sqnum` that bind the PEB to a specific LEB of a specific
    volume. Acts as the **commit point** for a write — the new
    mapping becomes visible only after the VID header is written.

Device header
    The 32-byte header on each reserved PEB describing the overall
    UBI device (`vol_count`, `revision`, `vol_id_watermark`). Stored
    in dual-bank mirrors.

Volume header
    A 48-byte header describing one volume (`name`, `type`,
    `leb_count`, `volume_id`). Stored sequentially after the device
    header on the reserved PEBs, mirrored across the active reserved
    bank.

EBA
    **Erase Block Association.** The per-volume mapping table from
    `lnum` (logical) to PEB index (physical). Stored in RAM as a
    red-black tree; rebuilt at attach time from the VID headers
    found on flash.

Reserved PEB
    A PEB at the start of the partition that holds device + volume
    metadata, never user data. UBI keeps two reserved PEBs **active**
    (true dual-bank mirrors) and may keep additional ones as cold
    spares (`CONFIG_UBI_DEV_HDR_NR_OF_RES_PEBS`).

Cold spare
    A reserved PEB held in reserve, erased and unused, that is
    promoted to active on demand when one of the active reserved PEBs
    fails permanently.

Free PEB
    A PEB that has been erased and carries a valid EC header but no
    VID header. Tracked in the `free_pebs` red-black tree, keyed by
    EC. The least-worn free PEB is selected for the next write.

Dirty PEB
    A PEB that holds stale data (overwritten LEB or unmapped LEB).
    Must be erased before re-use. Tracked in the `dirty_pebs`
    red-black tree, also keyed by EC. Reclaimed by
    `ubi_device_erase_peb()`.

Bad PEB
    A PEB that has failed an I/O operation (invalid EC header, write
    failure, erase failure). Excluded from the free / dirty pools.
    The bad-PEB list is **not** persisted to flash; it is rebuilt
    from the next attach scan.

Torture
    A short erase / verify loop applied to a candidate bad PEB to
    decide whether the failure was transient. If a PEB survives the
    torture cycles configured by
    `CONFIG_UBI_BAD_PEB_TORTURE_*`, it returns to the free pool;
    otherwise it stays bad.

Attach
    The act of mounting a UBI partition with `ubi_device_init()`. UBI
    scans every PEB, classifies it, rebuilds the EBA tables from VID
    headers, and (in Secure mode) consults the freshness store before
    enabling writes.

Sequence number
    See {term}`vid_sqnum` and {term}`global_sqnum`.

vid_sqnum
    The 64-bit monotonic sequence number stored in every VID header.
    Used at attach time to break ties when two PEBs claim the same
    `(volume_id, lnum)` — the higher `vid_sqnum` wins, the other PEB
    becomes dirty.

global_sqnum
    The device-wide maximum `vid_sqnum`, recovered at attach time as
    the highest value found across all VID headers. The next write
    uses `global_sqnum + 1`.

volume_id
    The durable, monotonically allocated identifier for a volume.
    Stored in the volume header, in every VID header that references
    the volume, and (in Secure mode) used as a stable cryptographic
    identity input for the per-volume LEB key. **Never reused** for
    the lifetime of one formatted device, even after
    `ubi_volume_remove`.

vol_id_watermark
    The 32-bit monotonic counter held in the device header. Bumped
    by every `ubi_volume_create` and never decremented. When it
    reaches `UINT32_MAX`, no further volumes can be created.

Dual-bank
    The crash-safe storage discipline UBI applies to reserved PEBs:
    two active reserved PEBs hold byte-identical mirrors of the
    device + volume headers. A metadata update writes both copies
    sequentially; if a crash splits the write, attach picks the
    survivor with the higher `revision`.

Reserved generation
    One end-to-end reserved-area write cycle. Every metadata change
    advances the device-header `revision` field; the active reserved
    PEBs are then rewritten in lock-step.

Read-only degraded mode
    The state UBI enters when reserved-PEB redundancy is lost (one
    active reserved PEB plus zero spares). Mutators return `-EROFS`;
    reads and `ubi_device_erase_peb()` keep working, the latter
    attempting self-healing on every call.

Static volume
    A volume created with `UBI_VOLUME_TYPE_STATIC`. Its `leb_count`
    is fixed at create time. `ubi_leb_unmap()` is forbidden on
    static volumes (returns `-EACCES`).

Dynamic volume
    A volume created with `UBI_VOLUME_TYPE_DYNAMIC`. `leb_count`
    can be changed at runtime via `ubi_volume_resize()`.

IKM
    **Input Keying Material.** The per-version root secret of Secure
    UBI, denoted `IKM[v]` where `v` is the `key_version`. Must be
    ≥ 256 bits, device-unique, and provided as a PSA key. Never
    reaches UBI as raw bytes.

PRK
    **Pseudo-Random Key.** The HKDF-Extract output of `IKM[v]`. UBI
    HKDF-Expands it with fixed domain labels to obtain the per-domain
    child keys (`K_device_header`, `K_volume_header`, `K_erase_counter`,
    `K_volume_identifier`, `K_leb`).

key_version
    An 8-bit identifier for one root key generation. Carried in
    every secure record's prefix. The application-supplied
    {term}`allowlist` enumerates the versions UBI may authenticate.

write-active key version
    The single `key_version` UBI uses for **new** secure writes. Held
    in the secure device header. Monotonic — never moves backward on
    the same formatted device.

Allowlist
    The application-supplied array `allowed_key_versions[]`. UBI
    authenticates on-flash records only if their `key_version`
    appears here; otherwise it emits
    `UBI_CRYPTO_EVENT_KEY_VERSION_NOT_ALLOWLISTED` and rejects the
    record.

Freshness store
    External, application-owned trusted storage for the
    `(device_revision, global_sqnum)` pair UBI exports after a
    successful attach. Read by the `check_freshness` callback at
    attach time and updated by the optional `sync_freshness`
    callback after each commit-visible mutation. Without one, Secure
    UBI cannot detect rollback to an earlier authentic state.

Hidden anchor
    A per-volume reserved data PEB Secure UBI keeps so that
    per-volume LEB usage floors survive `unmap`, `shrink`, and
    `volume_remove`. Invisible to the application.

KEY_RETIRABLE
    The Secure UBI event emitted when a non-write-active key version
    has on-flash refcount zero, meaning every PEB authenticated under
    that key has been erased or overwritten. Only at this point is it
    safe for the application to remove the version from the
    {term}`allowlist` and destroy its PSA key material.

Strict read-only
    A sticky read-only state Secure UBI may latch when an event
    breaches policy (`STRICT_RO_ON_RNG_FAILURE`,
    `STRICT_RO_ON_POLICY_FAILURE`,
    `STRICT_RO_ON_FRESHNESS_SYNC_FAILURE`). Cleared only by
    `ubi_device_deinit()`.

PSA Crypto
    The Arm Platform Security Architecture cryptography API. Secure
    UBI consumes PSA Crypto exclusively (`AES-128-CCM`,
    `HKDF-SHA-256`, CSPRNG); it never receives raw key bytes.

Erased value
    The byte value the flash hardware reports for an erased cell.
    UBI reads this at runtime via `flash_area_erased_val()` and never
    hardcodes `0xFF` (NOR) or `0x00`.
```

## See also

- {doc}`/getting_started/concepts` — the same six core terms (PEB,
  LEB, EC, VID, EBA) introduced with worked examples.
- {doc}`/architecture/plain_architecture` — the canonical reference
  for the plain on-flash layout and recovery rules.
- {doc}`/reference/onflash_format_spec` — the canonical reference for
  Secure UBI byte layouts and lifecycle invariants.

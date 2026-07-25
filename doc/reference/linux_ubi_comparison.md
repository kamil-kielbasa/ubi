# Linux UBI API comparison

Zephyr UBI is a from-scratch port of the model behind Linux's `drivers/mtd/ubi`
— PEBs and LEBs, EC/VID headers, an EBA mapping, global wear-leveling, bad-block
handling, and sequence-number recovery. It deliberately implements a **subset**
of the Linux in-kernel UBI contract (`include/linux/mtd/ubi.h`), shaped for
Zephyr's `flash_area` API and MCU resource constraints.

This page maps the two APIs operation-by-operation so anyone familiar with Linux
UBI knows exactly what carries over, what was renamed, and what is intentionally
absent.

## The one naming subtlety worth internalising

Linux splits LEB mutation into **two** operations:

| Linux operation | Meaning |
| --- | --- |
| `ubi_leb_write(desc, lnum, buf, offset, len)` | Incremental, in-place, program-only write at increasing `offset`. Dynamic volumes only. Not power-fail atomic. |
| `ubi_leb_change(desc, lnum, buf, len)` | Atomic whole-LEB replace (fresh PEB, write, swap). |

Zephyr UBI provides **both**, but the names line up differently:

- **`ubi_leb_write(ubi, vol_id, lnum, buf, len)`** is the *atomic whole-LEB
  replace* — i.e. it corresponds to Linux's **`ubi_leb_change`**. Every call
  allocates a fresh PEB, writes the full payload, records `data_size` in the VID
  header, and atomically swaps the LEB's mapping.
- **`ubi_leb_write_at(ubi, vol_id, lnum, offset, buf, len)`** is the
  *offset-based in-place append* — i.e. it corresponds to Linux's
  **`ubi_leb_write`**. It programs directly into the LEB's currently mapped PEB
  without relocation, so a LEB can be built up across several calls. Not
  power-fail atomic, dynamic volumes only — matching the Linux contract.

If you are porting Linux code: `ubi_leb_change` → `ubi_leb_write`, and
`ubi_leb_write(...offset...)` → `ubi_leb_write_at`.

## Full operation mapping

| Linux UBI contract | Zephyr UBI | Status |
| --- | --- | --- |
| `ubi_leb_change` (atomic replace) | `ubi_leb_write` | ✅ match (renamed) |
| `ubi_leb_write` (offset append, dynamic) | `ubi_leb_write_at` | ✅ match (renamed) |
| `ubi_leb_read(…, offset, len, check)` | `ubi_leb_read(…, offset, …)` | ⚠️ no `check` flag; see *read bounds* below |
| unmapped LEB reads as all-`0xff` | reads of a `data_size == 0` LEB return `0xff` for unwritten regions | ✅ (for in-place / mapped-empty LEBs) |
| `ubi_leb_map` / `ubi_leb_unmap` / `ubi_is_mapped` | `ubi_leb_map` / `ubi_leb_unmap` / `ubi_leb_is_mapped` | ✅ match |
| `ubi_leb_erase` (unmap + synchronous erase, returns EC) | split: `ubi_leb_unmap` + `ubi_device_erase_peb` | ⚠️ no single per-LEB synchronous erase |
| `ubi_leb_get_size` (via static `data_size`) | `ubi_leb_get_size` | ⚠️ returns stored `data_size` (0 for in-place LEBs) |
| `ubi_create_volume` / `ubi_remove_volume` | `ubi_volume_create` / `ubi_volume_remove` | ✅ match |
| `ubi_rsvol` (resize, dynamic only) | `ubi_volume_resize` (dynamic only) | ✅ match |
| `ubi_rename_volumes` | — | ❌ not implemented |
| `ubi_open_volume` + modes (RO/RW/EXCLUSIVE/METAONLY) | `(device, vol_id)` + per-device mutex | ❌ no handle/mode/locking model |
| `ubi_sync` / `ubi_flush` (deferred work) | — (writes are synchronous) | ❌ not needed |
| STATIC = immutable after update, whole-volume CRC | STATIC is writable like DYNAMIC | ❌ semantics not enforced |
| Global wear-leveling pool | same | ✅ concept match |
| Transparent bad-block handling | same, plus torture-test confirmation | ✅ match, stricter |
| Redundant volume table + `sqnum` recovery | device/volume headers in dual-bank reserved PEBs, `global_sqnum` | ✅ concept match |
| `leb_size = peb − EC_hdr − VID_hdr`, min-I/O alignment | `erase_block_size − UBI_EC_HDR_SIZE − UBI_VID_HDR_SIZE` | ✅ match |
| Active wear-leveling / scrubbing (relocate on correctable ECC) | passive WL only (EC-tracked free pool + dirty reclamation) | ⚠️ no live-data relocation |

## Read bounds and `data_size`

Every Zephyr write stamps `data_size` into the VID header and reads are validated
against it — this is Linux's **static-volume** behaviour, generalised to all
whole-LEB writes. A Zephyr LEB therefore behaves like a *rewritable static LEB*:
integrity-checked, fixed-length-per-write, atomically replaced.

`ubi_leb_write_at` opts out of that binding: it maps with `data_size == 0`, which
is treated as "no bound length" — the whole LEB is addressable and unwritten
regions read back as `0xff`, exactly like a Linux dynamic-volume LEB whose used
length is tracked by the upper layer rather than by UBI.

## Intentional omissions

Relative to Linux UBI, Zephyr UBI does not (currently) implement: volume open
handles and access modes, `ubi_rename_volumes`, `ubi_sync`/`ubi_flush` (all
writes are synchronous), a single per-LEB synchronous `ubi_leb_erase`,
static-volume immutability enforcement, and active wear-leveling / scrubbing.
None of these change the on-flash format; they are additive if a use case needs
them.

## Secure mode

The secure backend (`CONFIG_UBI_SECURE`) AEAD-wraps every commit-visible
structure, including LEB payloads, with `data_size` bound into the AAD. In-place
partial update is therefore not expressible there — `ubi_leb_write_at` returns
`-ENOSYS` on the secure backend. Whole-LEB `ubi_leb_write` (atomic replace) and
all other operations behave as in plain mode.

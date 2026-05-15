# Plain UBI Workflow

**What this page covers:** how to wire plain (non-encrypted) UBI into
your application — when to use it, the lifecycle of a UBI handle,
volume management, the LEB read/write path, the garbage-collection
contract, error and degraded-mode handling, and clean teardown.

**Prerequisites:** {doc}`/getting_started/concepts` for the vocabulary
(PEB, LEB, EC, VID, EBA) and {doc}`/getting_started/quick_start` if
you want a 5-minute hands-on session first.

**After reading this:** you will know exactly what each public call
does at the right level of abstraction to integrate UBI into a real
application — without having to reverse-engineer the API page or the
internals.

---

## 1. When to use plain UBI

Plain UBI is the right choice when:

- you store application data on raw NOR or NAND flash and need
  wear-leveling and crash-safe metadata;
- the data **does not** require confidentiality or authenticity
  beyond CRC integrity;
- the platform has no PSA Crypto / mbedTLS, or the secure cost
  (~20 KB extra flash) is unjustified;
- you want a deterministic, dependency-free volume layer that pairs
  cleanly with a higher-level filesystem (LittleFS, FAT) or with
  application-managed records.

If any of those conditions does not hold and you need authenticated
encryption of every on-flash record, read
{doc}`/guide/secure_workflow` instead.

## 2. Prerequisites checklist

Before your first plain UBI build:

| Prerequisite | Where it lives | How to verify |
|---|---|---|
| `CONFIG_UBI_ENABLE=y` | Kconfig | Build pulls in `lib..__ubi__lib.a` |
| Flash partition declared in DeviceTree (`fixed-partitions` with a `label` UBI knows) | `*.overlay` | `flash_area_open()` succeeds for that partition |
| Static or heap memory backend chosen | Kconfig (`CONFIG_UBI_MEM_BACKEND_*`) | See {doc}`/guide/configuration` § *Memory Backend* |
| Pool sizes scaled to the partition | `CONFIG_UBI_MAX_NR_OF_DATA_PEBS`, `CONFIG_UBI_MAX_NR_OF_VOLUMES` | `ubi_device_init()` returns `0` (not `-ENOMEM`) |
| Garbage-collection trigger (workqueue, periodic timer, or app-level call site) | Application code | Dirty-PEB count stays bounded under sustained writes |

## 3. Lifecycle of a UBI handle

Every interaction starts with `ubi_device_init()` and ends with
`ubi_device_deinit()`. Between those two calls the returned
`struct ubi_device *` is the single handle through which all
operations on that flash partition are routed.

```c
#include <ubi.h>

static struct ubi_device *ubi;

int storage_init(void)
{
    struct ubi_flash_desc flash = {
        .partition_id = FIXED_PARTITION_ID(ubi_partition),
    };

    int ret = ubi_device_init(&flash, NULL, &ubi);
    if (ret != 0) {
        return ret;
    }
    return 0;
}

void storage_deinit(void)
{
    ubi_device_deinit(ubi);
    ubi = NULL;
}
```

Notes:

- The third argument to `ubi_device_init()` is the runtime selector
  for the secure backend. Pass `NULL` for plain UBI.
- A single partition admits **one** active handle at a time; a second
  `ubi_device_init()` for the same `partition_id` returns `-EBUSY`.
- `ubi_device_init()` is the only call that performs the full PEB
  scan. Subsequent operations work entirely from the in-RAM cache.
- On a brand-new partition, init also performs a one-time format:
  it writes the device header to the reserved PEBs and an EC header
  with `ec=0` to every data PEB.

## 4. Creating, using, removing volumes

A UBI device hosts up to `CONFIG_UBI_MAX_NR_OF_VOLUMES` independent
volumes. Each volume is a named, sized container of LEBs.

```c
struct ubi_volume_config cfg = {
    .name      = "settings",
    .type      = UBI_VOLUME_TYPE_DYNAMIC,
    .leb_count = 4,
};

int vol_id;
int ret = ubi_volume_create(ubi, &cfg, &vol_id);
if (ret == 0) {
    /* vol_id is the durable identifier; persist or rediscover it. */
}
```

- **Static volumes** (`UBI_VOLUME_TYPE_STATIC`) freeze their
  `leb_count` at create time. Best for fixed assets like firmware
  slots in an A/B layout.
- **Dynamic volumes** (`UBI_VOLUME_TYPE_DYNAMIC`) accept later
  `ubi_volume_resize()`. Best for application data whose size
  changes over time.
- `ubi_volume_create()` is **idempotent** when called with the same
  `name` and the same configuration — it returns `0` and the
  existing `vol_id`. With the same `name` but a different config it
  returns `-EEXIST`.
- Volume IDs are **monotonic and never reused**. After
  `ubi_volume_remove()` the slot is freed but the ID is gone for
  good. The 32-bit `vol_id_watermark` is large enough that this is
  only relevant in pathological create/remove loops.

Discovery after reboot:

```c
struct ubi_device_info di;
ubi_device_get_info(ubi, &di);
for (int i = 0; i < di.vol_count; i++) {
    /* enumerate by index — see Volume Management in the API ref */
}
```

Most applications instead persist the `vol_id` of each known volume
in their own settings store and look up `ubi_volume_get_info(ubi,
vol_id, &cfg, NULL)` on the next boot.

## 5. Reading and writing LEBs

UBI exposes raw LEB-level I/O. There is no notion of files, offsets
across LEBs, or partial-LEB metadata — that is the next layer up.

### Write

```c
int ret = ubi_leb_write(ubi, vol_id, /*lnum=*/0, payload, payload_len);
```

What happens internally:

1. The least-worn free PEB is selected (`rb_get_min(free_pebs)`).
2. The new PEB is fully written: EC header → user data → VID header.
   The VID header is the **commit point** — the new mapping is
   visible only after that final write succeeds.
3. The old PEB (if any) is moved to the dirty pool for later reclaim.
4. On flash failure the new PEB is marked bad and step 1 retries
   with the next-best free PEB; the old mapping is untouched.

This copy-on-write contract guarantees: a write either takes effect
in full or leaves the previous content intact. There is no
"half-written LEB" state visible to the caller.

### Read

```c
int ret = ubi_leb_read(ubi, vol_id, /*lnum=*/0, /*offset=*/0,
                       buf, sizeof(buf));
```

Reads bypass the dirty / bad pools and go directly through the EBA
table. An unmapped LEB returns `-ENOENT`. Reads do not advance
sequence numbers or touch flash beyond the data area.

### Map / unmap

`ubi_leb_map()` writes only the EC + VID headers and leaves the data
area erased — useful when you want to reserve a LEB without payload.
`ubi_leb_unmap()` releases the mapping (the underlying PEB becomes
dirty). Unmap is **forbidden on static volumes** and returns
`-EACCES`.

## 6. Garbage collection

UBI does **not** run an internal background thread. The application
drives reclaim by calling `ubi_device_erase_peb()`. One PEB is
processed per call:

- pick the dirty PEB with the lowest erase counter;
- erase it on flash;
- bump its EC, write a fresh EC header, return it to the free pool;
- on erase failure, mark the PEB bad and return `0`.

A typical wiring is a periodic Zephyr work item — see
{doc}`/guide/cookbook` § *Periodic garbage collection in a workqueue*
for the runnable recipe. The right cadence is application-specific:

- **Bursty writes** (config snapshots, journal flushes): call
  `ubi_device_erase_peb()` immediately after the burst.
- **Steady-state writes** (sensor logs): a 1 Hz workqueue is usually
  enough and is cheap when there is nothing to do (the call returns
  `0` instantly when `dirty_pebs` is empty).
- **Tight free-PEB headroom**: trigger reclaim from the
  `-ENOSPC` error path of your write code as a fallback.

The cost of one `erase_peb()` call is dominated by the underlying
flash erase latency (milliseconds on NOR, tens of milliseconds on
NAND). Schedule accordingly.

## 7. Error handling

All public functions return `0` on success or a negative `errno`.
The full enumeration is in {doc}`/reference/error_codes`; the codes
that drive normal application logic are:

| Code | Where you will see it | What it tells you |
|---|---|---|
| `-ENOENT` | reads, info queries | volume or LEB does not exist |
| `-EEXIST` | `ubi_volume_create` | name reused with different config |
| `-ENOSPC` | writes, create, resize | run `ubi_device_erase_peb()` then retry; if still `-ENOSPC` the partition is full |
| `-EROFS` | every mutator | device is in degraded read-only mode (see § 8); reads still work |
| `-EIO` | any flash-touching call | physical flash error; affected PEB will be marked bad |
| `-EINVAL` | every public function | programmer error — fix the caller, do not retry |

`-EBUSY` is reserved for `ubi_device_init()` and signals that
another handle on the same partition is already alive.

## 8. Degraded read-only mode

UBI keeps two active reserved PEBs (dual-bank) and up to two cold
spares (`CONFIG_UBI_DEV_HDR_NR_OF_RES_PEBS`, default 2 active + 0
spares = total 2). When a reserved PEB fails permanently and no
spare is available, the device drops into **degraded read-only**
mode. From that point on, every mutator returns `-EROFS`. Reads,
info queries, and `ubi_device_erase_peb()` still work.

Recovery is automatic: each `ubi_device_erase_peb()` call attempts
to rewrite the bad reserved bank from the surviving active PEB. On
success the `read_only_degraded` flag clears and writes resume.
Wiring the periodic GC work-item is therefore enough — no separate
recovery code is needed.

To check the flag explicitly:

```c
struct ubi_device_info di;
ubi_device_get_info(ubi, &di);
if (di.read_only_degraded) {
    LOG_WRN("UBI is in read-only mode, "
            "self-healing on next erase_peb() cycle");
}
```

## 9. Tear-down

`ubi_device_deinit()` acquires the per-device mutex, releases all
in-RAM structures (the static slabs are returned to their pools, the
heap structures are freed), and clears the partition guard so a
later `ubi_device_init()` can re-attach.

Make sure no other thread starts a new operation after you have
called deinit. Operations already in flight will complete before
deinit proceeds — they hold the mutex.

## 10. What's next

- {doc}`/guide/cookbook` — six runnable recipes (STM32U5, nRF5340,
  A/B firmware slots, GC workqueue, key rotation, freshness store).
- {doc}`/guide/configuration` — Kconfig sizing, DeviceTree partition
  layout, log levels.
- {doc}`/architecture/plain_architecture` — internals, header byte
  layouts, RBT data structures, recovery rules.
- {doc}`/reference/api` — per-function Doxygen reference.
- {doc}`/reference/error_codes` — full error-code enumeration.

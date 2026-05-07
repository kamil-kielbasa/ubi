# Cookbook

End-to-end recipes for common UBI on Zephyr deployments. Each recipe
is self-contained — copy it into your project, adjust the partition
geometry, and build.

**Audience:** application authors who already understand the basics
({doc}`what_is_ubi`, {doc}`concepts`, {doc}`quick_start`).

**See also:** {doc}`plain_workflow`, {doc}`secure_workflow`,
{doc}`configuration`.

```{contents}
:local:
:depth: 1
```

---

## 1. UBI on STM32U5 (`b_u585i_iot02a`)

**Goal:** carve a 128 KiB UBI partition out of the on-chip flash and
attach it.

**Board overlay** (`boards/b_u585i_iot02a.overlay`):

```dts
&flash0 {
    partitions {
        compatible = "fixed-partitions";
        #address-cells = <1>;
        #size-cells = <1>;

        ubi_partition: partition@d0000 {
            label = "ubi_partition";
            reg = <0x000d0000 DT_SIZE_K(128)>;
        };
    };
};
```

**`prj.conf` essentials:**

```kconfig
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_FLASH_PAGE_LAYOUT=y
CONFIG_MPU_ALLOW_FLASH_WRITE=y
CONFIG_CRC=y
CONFIG_UBI_ENABLE=y
```

**Build & flash:**

```bash
west build -p always -b b_u585i_iot02a sample/
west flash
```

**Sanity check:** the sample writes "Hello, UBI!" to a one-LEB volume,
reads it back, and exits. With `CONFIG_LOG=y` you should see
`UBI initialization` and the LEB write/read messages on the console.

**Geometry note.** STM32U5 page size is 8 KiB → 128 KiB partition gives
**16 PEBs**. Reserve 4 for dual-bank metadata (default), leaving
**12 PEBs** for user data and wear-leveling headroom. If you need
larger volumes, grow the partition rather than reducing the reserved
bank count.

---

## 2. UBI on nRF5340

**Goal:** carve a 64 KiB UBI partition on the application core.

**Board overlay** (`boards/nrf5340dk_nrf5340_cpuapp.overlay`):

```dts
&flash0 {
    partitions {
        compatible = "fixed-partitions";
        #address-cells = <1>;
        #size-cells = <1>;

        ubi_partition: partition@f0000 {
            label = "ubi_partition";
            reg = <0x000f0000 DT_SIZE_K(64)>;
        };
    };
};
```

**`prj.conf` essentials:** identical to recipe 1.

**Build & flash:**

```bash
west build -p always -b nrf5340dk/nrf5340/cpuapp sample/
west flash
```

**Geometry note.** nRF5340 page size is 4 KiB → 64 KiB partition gives
**16 PEBs**. With the default 4-PEB reserved bank that leaves
**12 PEBs** of usable storage. The radio core has its own flash
partitioning; UBI runs entirely on the application core.

---

## 3. A/B firmware slots with two static volumes

**Goal:** keep two firmware images side by side in a single UBI
partition so the bootloader can pick whichever slot is healthy.

**Why two volumes?** Each volume is independently sized, named, and
crash-recovered. A single UBI partition with two `STATIC` volumes
gives you A/B semantics without the bootloader having to know about
flash geometry — it sees stable logical names.

```c
#include <ubi.h>

#define LEB_PER_SLOT 6   /* tune to your image size */

static int provision_ab_slots(struct ubi_device *ubi)
{
    struct ubi_volume_config slot_a = {
        .name      = "fw_a",
        .type      = UBI_VOLUME_TYPE_STATIC,
        .leb_count = LEB_PER_SLOT,
    };
    struct ubi_volume_config slot_b = {
        .name      = "fw_b",
        .type      = UBI_VOLUME_TYPE_STATIC,
        .leb_count = LEB_PER_SLOT,
    };

    int vol_a = -1, vol_b = -1;
    int ret;

    ret = ubi_volume_create(ubi, &slot_a, &vol_a);
    if (ret != 0 && ret != -EEXIST) {
        return ret;
    }
    ret = ubi_volume_create(ubi, &slot_b, &vol_b);
    if (ret != 0 && ret != -EEXIST) {
        return ret;
    }
    return 0;
}
```

**Update flow (sketch):**

1. Bootloader runs from the slot it last marked `active`.
2. Application receives a new image, picks the **inactive** slot, and
   streams LEBs:

   ```c
   for (size_t lnum = 0; lnum < image_leb_count; ++lnum) {
       ubi_leb_write(ubi, vol_inactive, lnum,
                     image_chunk[lnum], image_chunk_size);
   }
   ```

3. After the last LEB lands, application flips an "active slot"
   marker stored in a separate small volume (e.g., `fw_meta`).
4. Reboot. Bootloader reads `fw_meta`, attaches the indicated slot.

**Failure model.** A power loss mid-update leaves the inactive slot
in an indeterminate state; the active slot is untouched (UBI's
LEB-to-PEB mapping is updated atomically per LEB). Roll forward by
retrying the update; nothing in the active slot moves until the
`fw_meta` flip.

---

## 4. Periodic garbage collection in a workqueue

**Goal:** keep the dirty-PEB pool from filling up under sustained
write traffic without blocking application code.

UBI does **not** run an internal background thread. The application
drives reclaim by calling `ubi_device_erase_peb()`. One PEB is
reclaimed per call — you decide cadence and priority.

```c
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <ubi.h>

LOG_MODULE_DECLARE(app);

#define GC_PERIOD_MS 1000

static struct ubi_device *ubi_dev;
static void ubi_gc_work_handler(struct k_work *work);

K_WORK_DELAYABLE_DEFINE(ubi_gc_work, ubi_gc_work_handler);

static void ubi_gc_work_handler(struct k_work *work)
{
    int ret = ubi_device_erase_peb(ubi_dev);
    if (ret != 0) {
        LOG_WRN("ubi_device_erase_peb returned %d", ret);
    }
    k_work_schedule(&ubi_gc_work, K_MSEC(GC_PERIOD_MS));
}

void ubi_gc_start(struct ubi_device *ubi)
{
    ubi_dev = ubi;
    k_work_schedule(&ubi_gc_work, K_MSEC(GC_PERIOD_MS));
}
```

**Tuning.**

- A no-op call (no dirty PEBs) returns `0` immediately. A 1 Hz cadence
  is therefore cheap when the system is idle.
- Under heavy write load, lower the period (e.g. 100 ms) or call
  `ubi_device_erase_peb()` from the producer's own workqueue to
  back-pressure writes.
- This same loop also drives **degraded read-only recovery**: a
  transient reserved-bank fault can self-heal across a few GC ticks
  without a reboot (see `ubi_device_erase_peb()` in {doc}`api`).

---

## 5. Implementing key rotation with PSA

**Goal:** roll the Secure UBI write-active key from version 1 to
version 2 without losing data and without spuriously retiring the
old key.

**Prerequisites:** read {doc}`secure_workflow` §5 for the conceptual
workflow. This recipe shows the application-side glue.

```c
#include <ubi.h>
#include <ubi_crypto.h>
#include <psa/crypto.h>

static psa_key_id_t key_v1;   /* legacy root */
static psa_key_id_t key_v2;   /* freshly provisioned root */

static int my_get_key_id(uint8_t key_version, uint32_t *out)
{
    switch (key_version) {
    case 1: *out = (uint32_t)key_v1; return 0;
    case 2: *out = (uint32_t)key_v2; return 0;
    default: return -ENOENT;
    }
}

static enum ubi_crypto_event_verdict
my_event_cb(const struct ubi_crypto_event *ev, void *user_data)
{
    if (ev->type == UBI_CRYPTO_EVENT_KEY_RETIRABLE) {
        /* Refcount of the named key version reached zero on flash.
         * Now (and only now) is it safe to destroy the PSA key.
         */
        if (ev->key_version == 1) {
            psa_destroy_key(key_v1);
            key_v1 = PSA_KEY_ID_NULL;
        }
    }
    return UBI_CRYPTO_EVENT_CONTINUE;
}
```

**Rotation steps:**

1. Provision `key_v2` (e.g. `psa_generate_key()` into a hardware-bound
   slot) and add it to the allowlist:

   ```c
   static const uint8_t allowed[] = { 1, 2 };

   struct ubi_crypto_config crypto_cfg = {
       .policy = {
           .requested_write_key_version = 2,
           .allowed_key_versions        = allowed,
           .allowed_key_versions_len    = ARRAY_SIZE(allowed),
       },
       .get_key_id      = my_get_key_id,
       .check_freshness = my_check_freshness,
       .sync_freshness  = my_sync_freshness,
       .event_cb        = my_event_cb,
       .user_data       = NULL,
   };
   ```

2. Reattach the device with the new config. Secure UBI starts
   writing under `v=2`; existing live mappings stay under `v=1`.
3. Either let normal traffic age `v=1` out (lazy), or — for
   compromise response — drive your GC loop hard until refcounts
   reach zero (forced; see {doc}`secure_workflow` §5.2).
4. Wait for the `KEY_RETIRABLE` event for `v=1`. The handler above
   destroys the PSA key.
5. On the next reattach, drop `1` from `allowed[]`.

**Pitfall.** Do not call `psa_destroy_key()` on `v=1` before
`KEY_RETIRABLE`. Stale on-flash references to `v=1` would then become
unreadable and trigger `KEY_VERSION_UNAVAILABLE` events.

---

## 6. Implementing a freshness store with Zephyr Settings

**Goal:** give Secure UBI a trusted reference for `(device_revision,
global_sqnum)` so it can detect rollback to an older authentic state.

**Why Zephyr Settings?** It lives outside the UBI partition, survives
reboot, and is written through the same flash backend. For
production, back this with TF-M secure storage or a hardware
monotonic counter; the structure of the callbacks does not change.

```c
#include <zephyr/settings/settings.h>
#include <ubi_crypto.h>

#define FRESHNESS_KEY "ubi/freshness"

struct stored_freshness {
    uint32_t device_revision;
    uint64_t global_sqnum;
};

static struct stored_freshness fr_state;

static int fr_settings_set(const char *name, size_t len,
                           settings_read_cb read_cb, void *cb_arg)
{
    if (len != sizeof(fr_state)) {
        return -EINVAL;
    }
    return read_cb(cb_arg, &fr_state, sizeof(fr_state));
}

SETTINGS_STATIC_HANDLER_DEFINE(ubi_fr, "ubi", NULL,
                               fr_settings_set, NULL, NULL);

void ubi_freshness_init(void)
{
    settings_subsys_init();
    settings_load_subtree("ubi");
}

static enum ubi_crypto_rollback_verdict
my_check_freshness(const struct ubi_crypto_freshness *fr, void *user_data)
{
    if (fr_state.device_revision == 0 && fr_state.global_sqnum == 0) {
        /* First boot: trust the on-flash value, persist, accept. */
        fr_state.device_revision = fr->device_revision;
        fr_state.global_sqnum    = fr->global_sqnum;
        settings_save_one(FRESHNESS_KEY, &fr_state, sizeof(fr_state));
        return UBI_CRYPTO_ROLLBACK_ACCEPT;
    }

    if (fr->device_revision  < fr_state.device_revision ||
        (fr->device_revision == fr_state.device_revision &&
         fr->global_sqnum    < fr_state.global_sqnum)) {
        return UBI_CRYPTO_ROLLBACK_REJECT;   /* rollback detected */
    }

    fr_state.device_revision = fr->device_revision;
    fr_state.global_sqnum    = fr->global_sqnum;
    settings_save_one(FRESHNESS_KEY, &fr_state, sizeof(fr_state));
    return UBI_CRYPTO_ROLLBACK_ACCEPT;
}

static int my_sync_freshness(const struct ubi_crypto_freshness *fr,
                             void *user_data)
{
    fr_state.device_revision = fr->device_revision;
    fr_state.global_sqnum    = fr->global_sqnum;
    return settings_save_one(FRESHNESS_KEY, &fr_state, sizeof(fr_state));
}
```

**Tuning.**

- `sync_freshness` is throttled by
  `CONFIG_UBI_CRYPTO_FRESHNESS_SYNC_DELTA` — Secure UBI calls it only
  after the on-flash counter has advanced by at least that many
  units. Pick a value that bounds your worst-case rewind window
  against the cost of a Settings write.
- If you cannot afford any rewind window, set the delta to `1`. That
  is one Settings write per commit-visible mutation.

**Pitfall.** If `settings_save_one()` fails, `sync_freshness` must
return non-zero so Secure UBI emits `FRESHNESS_SYNC_FAILURE`. Silently
swallowing the error defeats rollback detection.

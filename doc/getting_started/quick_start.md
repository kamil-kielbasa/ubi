# Quick Start

**What this page covers:** Build UBI, run the sample on the simulator, and
write your first volume — in about 5 minutes.

**Prerequisites:** A working
[Zephyr development environment](https://docs.zephyrproject.org/latest/develop/getting_started/index.html)
(`west`, Zephyr SDK).

**After reading this:** You will have UBI built and running. For deeper
integration on real hardware, continue to {doc}`/guide/configuration` and the
{doc}`/guide/cookbook`.

---

## Choose your path

| Path | What you do | What you need |
|------|-------------|---------------|
| **Quick evaluation** | Build for `native_sim`, run the sample, see UBI in action | Zephyr SDK only |
| **Project integration** | Add UBI as a west module, configure via Kconfig and DeviceTree | See {doc}`/guide/configuration` |
| **Hardware testing** | Build for `b_u585i_iot02a` or `nrf5340dk/nrf5340/cpuapp`, flash, observe via serial | STM32CubeProgrammer or nrfjprog, picocom |

This page covers the **Quick evaluation** path end to end. The other two
paths share the same first two steps (initialise the workspace; build).

## 1. Initialise the workspace

```sh
west init -l .
west update --narrow -o=--depth=1
```

## 2. Build and run the sample on `native_sim`

```sh
west build -p --build-dir build/native_sim/sample -b native_sim ./sample/
./build/native_sim/sample/zephyr/zephyr.exe
```

You should see UBI scan the simulated flash, create a volume, write a
record, read it back, and exit cleanly.

## 3. Your first volume — the 30-line version

The snippet below is a self-contained `main()` showing the four-call
lifecycle: init → create volume → write/read LEB → deinit. Error handling
is omitted for clarity; every API call returns `0` on success or a
negative `errno`.

```c
#include <ubi.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

#define UBI_PARTITION_NAME    ubi_partition
#define UBI_PARTITION_DEVICE  FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)

int main(void)
{
    const struct device *flash_dev = UBI_PARTITION_DEVICE;
    struct flash_pages_info page_info = { 0 };
    flash_get_page_info_by_offs(flash_dev, 0, &page_info);

    struct ubi_flash_desc flash = {
        .partition_id     = FIXED_PARTITION_ID(UBI_PARTITION_NAME),
        .erase_block_size = page_info.size,
        .write_block_size = flash_get_write_block_size(flash_dev),
    };

    struct ubi_device *ubi = NULL;
    ubi_device_init(&flash, NULL, &ubi);

    struct ubi_volume_config cfg = {
        .name      = "my_vol",
        .type      = UBI_VOLUME_TYPE_DYNAMIC,
        .leb_count = 4,
    };
    int vol_id;
    ubi_volume_create(ubi, &cfg, &vol_id);

    const char data[] = "Hello, UBI!";
    ubi_leb_write(ubi, vol_id, 0, data, sizeof(data));

    char buf[64];
    ubi_leb_read(ubi, vol_id, 0, 0, buf, sizeof(buf));

    ubi_device_deinit(ubi);
    return 0;
}
```

A complete buildable version of this snippet ships in
[`sample/`](https://github.com/kamil-kielbasa/ubi/tree/main/sample).

## What's next

- {doc}`/getting_started/concepts` — the vocabulary used throughout the rest of the docs
  (PEB, LEB, EC, VID, EBA).
- {doc}`/guide/configuration` — Kconfig, DeviceTree overlays, and the memory
  sizing guide.
- {doc}`/guide/plain_workflow` — full lifecycle walkthrough including periodic
  garbage collection and `read_only_degraded` handling.
- {doc}`/guide/cookbook` — end-to-end recipes for common deployments.
- {doc}`/project/test_strategy` — how to build and run the test suite, and how
  coverage and forensic scans are produced.

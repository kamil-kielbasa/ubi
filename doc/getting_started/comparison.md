# Comparison: UBI vs LittleFS vs NVS vs ZMS

**What this page covers:** A side-by-side comparison of UBI and the
three main Zephyr-native storage subsystems you would otherwise reach
for: LittleFS, NVS, and ZMS. The goal is to make the "which one do I
pick?" decision in five minutes.

**After reading this:** You will know which storage abstraction
matches your project's shape — and, equally important, when UBI is the
*wrong* answer.

---

## At a glance

| Feature | **UBI** | **LittleFS** | **NVS** | **ZMS** |
|---|---|---|---|---|
| **Abstraction** | Logical eraseblock volumes | POSIX-like filesystem (files, dirs) | Key → value store | Key → value store |
| **Granularity** | Eraseblock (typ. 4–64 KB) | File / byte | Record (≤ entry size) | Record (≤ entry size) |
| **Wear-leveling** | Global, across all PEBs | Per-block, copy-on-write | Cycle through reserved sectors | Cycle through reserved sectors |
| **Multiple volumes / namespaces** | **Yes** — N named volumes per partition | One filesystem per partition | One namespace per partition | One namespace per partition |
| **Bad-block handling** | Automatic detection + isolation | Block-level, transparent | None (assumes NOR) | None (assumes NOR) |
| **Dynamic resize** | Yes (dynamic volumes) | n/a (filesystem) | n/a | n/a |
| **Crash safety (metadata)** | Dual-bank reserved PEBs + monotonic `vid_sqnum` | Copy-on-write, journaled | Sector-rotation atomic write | Sector-rotation atomic write |
| **Crash safety (user data)** | **No** — metadata only; mid-write loses the LEB | Yes — every write is atomic | Yes — record write is atomic | Yes — record write is atomic |
| **Authenticated encryption** | **Yes**, opt-in (`CONFIG_UBI_CRYPTO`, AES-128-CCM via PSA) | No | No | No |
| **Random-access I/O** | Yes, within a LEB | Yes (file `seek` + `read`/`write`) | Read by key | Read by key |
| **Library footprint (Cortex-M33, `-Os`)** | ~9.5 KB plain / ~29 KB secure | ~25–35 KB depending on config | ~3–5 KB | ~4–6 KB |
| **RAM cost** | Proportional to PEB count + volume count | File table + lookahead buffer | Two-sector cache | Cache + ATE table |
| **Tightly bound to file model** | No — block-level | Yes | No (KV) | No (KV) |
| **Target media** | Raw NOR / NAND | Raw NOR / NAND | Raw NOR | Raw NOR |
| **Origin** | Inspired by Linux UBI; adapted for Zephyr's resource-constrained targets | Upstream ARM project, integrated in Zephyr | Zephyr-native | Zephyr-native (successor of NVS) |

Numbers are order-of-magnitude. Exact footprint depends on Kconfig,
toolchain, and feature flags. UBI footprint figures come from the
sample build on the `b_u585i_iot02a` board (STM32U585) — see
{doc}`/architecture/plain_architecture` for the full methodology.

## Picking the right one

### Pick **UBI** when…

- You have **raw NOR or NAND flash** (not eMMC / SD).
- You need **multiple independent logical volumes** on the same
  partition — for example, an A/B firmware image pair plus a
  configuration blob, all sharing one wear-leveling pool.
- You need **global wear-leveling** that NVS/ZMS cannot offer because
  your records are large or eraseblock-shaped.
- You want a **predictable, fully static RAM cost** that you can size
  at compile time.
- You may need **authenticated encryption of every on-flash byte** at
  some point in the product lifetime (turn `CONFIG_UBI_CRYPTO` on
  later without rewriting your storage layer).

### Pick **LittleFS** when…

- You need a real **filesystem** with files, directories, and a POSIX
  API.
- Your access pattern is **fine-grained byte-level reads and writes**
  — typical for configuration files, logs, or many small artefacts.
- Power-loss atomicity for **user data** is a hard requirement.
- You are willing to accept a larger code footprint and the inherent
  overhead of a journaling filesystem.

LittleFS can also be layered **on top of UBI** when you want both
multi-volume wear-leveling and a filesystem inside one of those
volumes.

### Pick **NVS** when…

- You only need a **key → value** store for a small number of
  configuration items.
- The total data set fits comfortably inside one flash partition with
  a few reserved sectors.
- You want the **smallest possible code and RAM footprint**.

### Pick **ZMS** when…

- Your use case looks like NVS, but you also need **larger records**,
  **faster lookups** at scale, or the additional features ZMS adds
  over NVS.
- You are starting a new project today and Zephyr's documentation
  steers you toward ZMS as the modern successor to NVS.

### Don't pick any of the four when…

- Your storage is **managed flash** (eMMC, SD card). Use the native
  filesystem on top of the device's own FTL — every layer here would
  just duplicate work the device already does.
- Your write workload is **so light** that you can keep state in RAM
  and persist a single blob to a fixed flash partition through
  `flash_area_write()`. None of these layers buys you anything in that
  regime.

## Decision shortcut

A two-question shortcut that gets the right answer ~90 % of the time:

1. **Do I need files, directories, or a POSIX API?**
   - Yes → **LittleFS** (optionally on top of a UBI volume).
   - No → continue.
2. **Do I need more than one independent storage region on the same
   partition, or do I anticipate needing authenticated encryption of
   the entire device?**
   - Yes → **UBI** (plain or secure).
   - No → **ZMS** for new code, **NVS** if the project is already on
     NVS.

## See also

- {doc}`/getting_started/what_is_ubi` — UBI's own "non-goals" section
  re-states some of these boundaries from UBI's perspective.
- {doc}`/architecture/plain_architecture` — the on-flash format and
  resource-usage numbers behind the UBI row of the table above.
- {doc}`/architecture/secure_overview` — when authenticated encryption
  matters and what enabling `CONFIG_UBI_CRYPTO` costs.

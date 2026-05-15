# Concepts at a Glance

**What this page covers:** UBI's five key abstractions (PEB, LEB, EC, VID,
EBA) and the six steps that describe its runtime behaviour. This is the
shared vocabulary used throughout the rest of the documentation.

**Prerequisites:** {doc}`/getting_started/what_is_ubi` for the high-level pitch.

**After reading this:** You will recognise every term used in the
architecture, configuration, and API pages. For implementation depth,
continue to {doc}`/architecture/plain_architecture`.

---

## Five key concepts

| Concept | Abbreviation | What it is |
|---------|--------------|------------|
| Physical Erase Block | **PEB** | A hardware erase unit on the flash chip (e.g., 8 KB). The smallest unit that can be erased. |
| Logical Erase Block | **LEB** | A virtual block exposed to the application through a volume. Maps to exactly one PEB at a time. |
| Erase Counter | **EC** | A per-PEB counter tracking how many times that block has been erased. Used for wear-leveling decisions. |
| Volume Identifier | **VID** | A header on each data PEB that links it to a specific volume and LEB number. |
| Erase Block Association | **EBA** | The in-RAM mapping table from LEBs to PEBs. Rebuilt from VID headers during initialization. |

**The fundamental relationship.** An application writes to **LEBs**
(logical). UBI maps each LEB to a **PEB** (physical), choosing the
least-worn free block. The **VID** header on each PEB records which volume
and LEB it belongs to. The **EC** header records how many times it has
been erased.

## How it works in six steps

### 1. Initialization — scan and rebuild

UBI scans every PEB on the flash partition, reads EC and VID headers, and
rebuilds the in-RAM state: which PEBs are free, which belong to volumes,
which are dirty (stale), and which are bad.

### 2. Free, dirty, and bad pools

After scanning, each PEB is classified into exactly one pool:

| Pool | Meaning |
|------|---------|
| **Free** | Erased, ready for new writes. Stored in a red-black tree keyed by erase count. |
| **Mapped** | In use by a volume (tracked in that volume's EBA table). |
| **Dirty** | Contains stale data from a previous write. Needs erasure before reuse. |
| **Bad** | Failed I/O. Permanently excluded. |

### 3. Writing — pick the least-worn PEB

When writing to a LEB, UBI selects the free PEB with the **lowest erase
counter** (wear-leveling). It writes the EC header, VID header, and user
data to the new PEB, then updates the EBA mapping. If the LEB was
previously mapped, the old PEB moves to the dirty pool.

### 4. Reading — look up the EBA

Reading is a direct lookup: find the PEB mapped to the requested LEB in
the EBA table, then read from the correct offset on flash.

### 5. Reclaiming — erase dirty PEBs

Dirty PEBs are reclaimed by erasing them, incrementing their erase
counter, writing a new EC header, and moving them back to the free pool.
The application controls when this happens by calling
`ubi_device_erase_peb()`.

### 6. Recovery — sequence numbers resolve conflicts

If a power loss occurs mid-write, two PEBs may claim the same LEB. During
the next initialization, UBI compares their sequence numbers (`sqnum` in
the VID header) — the higher `sqnum` wins, and the loser is moved to
dirty.

## What's next

- {doc}`/getting_started/quick_start` — build, flash, write, read in 5 minutes.
- {doc}`/architecture/plain_architecture` — on-flash layout, header formats, in-RAM
  structures, and operation flows in detail.
- {doc}`/guide/configuration` — Kconfig, devicetree, and memory sizing guide.

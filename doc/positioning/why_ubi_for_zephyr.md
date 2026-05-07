# Why UBI matters on Zephyr

**Status:** positioning note  
**Scope:** plain UBI, secure UBI, and why they fill a real gap in Zephyr's storage stack  
**Audience:** Zephyr maintainers, reviewers, and application developers choosing a storage architecture

---

## 1. Short version

Zephyr already has useful storage subsystems:

- low-level flash partition access,
- FIFO and key-value storage,
- filesystems,
- PSA secure-storage APIs.

What Zephyr still does **not** provide as a first-class subsystem is a **general raw-flash volume manager and eraseblock virtualizer** for external or internal raw flash.

That is the gap UBI fills.

Plain UBI gives:

- logical volumes on raw flash,
- logical eraseblocks independent from physical placement,
- wear-leveling across the managed partition,
- crash-safe LEB-to-PEB remapping,
- bad-block isolation,
- metadata redundancy and deterministic recovery.

Secure UBI extends that same foundation with:

- confidentiality,
- authenticity,
- location binding,
- authenticated recovery state for future writes,
- exported freshness signals for application-level anti-rollback policy.

The result is not a filesystem. It is the layer **below** a filesystem, object store, or database engine.

---

## 2. What Zephyr provides today

Zephyr already covers several storage use cases well. The important point is that these subsystems solve **different layers of the stack**.

### 2.1 Flash map and flash area APIs

The [Flash map](https://docs.zephyrproject.org/latest/doxygen/html/group__flash__area__api.html) gives partition-level access to flash and exposes geometry details such as sector layout and write alignment.

That is exactly the right low-level API for UBI to build on.

What it does **not** provide by itself:

- logical volumes,
- remapping from logical eraseblocks to physical eraseblocks,
- wear-leveling policy,
- bad-block lifecycle,
- crash-safe metadata selection.

### 2.2 FCB

[FCB](https://docs.zephyrproject.org/latest/services/storage/fcb/fcb.html) provides a FIFO abstraction over flash.

That is useful for logs and append-only records.

What it does **not** try to be:

- a general multi-volume flash manager,
- a logical eraseblock abstraction,
- a reusable wear-leveling substrate for multiple higher layers.

### 2.3 NVS

[NVS](https://docs.zephyrproject.org/latest/services/storage/nvs/nvs.html) is a key-value store. It stores metadata and data in flash sectors, rotates sectors, and ensures that for each used key there is always at least one id-data pair present.

That is a strong fit for small configuration-style blobs.

What it does **not** aim to provide:

- multiple raw-flash volumes with independent sizing,
- a generic LEB/PEB abstraction for higher layers,
- a storage-manager substrate that a filesystem or database can reuse directly.

### 2.4 ZMS

[ZMS](https://docs.zephyrproject.org/latest/services/storage/zms/zms.html) is also a key-value store, designed to work across different non-volatile memory technologies. It has its own sector model, garbage collection, and wear-leveling behavior.

That makes it useful for application-facing KV storage.

What it still is not:

- a raw-flash volume manager,
- a general eraseblock virtualizer,
- a generic substrate for arbitrary upper storage engines.

### 2.5 File systems

Zephyr's [file-system layer](https://docs.zephyrproject.org/latest/services/storage/file_system/index.html) provides a VFS-style switch and supports multiple filesystems. It also allows external filesystems to be registered.

That is the correct layer when the application wants files and directories.

What a filesystem still needs underneath on raw flash is a sound flash-management strategy.

In some deployments the filesystem implements that itself. In others, a dedicated layer below the filesystem is cleaner.

### 2.6 Secure Storage / PSA ITS

Zephyr's [Secure Storage](https://docs.zephyrproject.org/latest/services/storage/secure_storage/index.html) provides an implementation of the PSA Secure Storage API on platforms that need one.

That is valuable, but its scope is different:

- it is an API-level secure-storage service,
- its own documentation explicitly says it does not aim at full PSA compliance,
- it does not guarantee secure-at-rest behavior in all configurations,
- and it explicitly says replay protection requires hardware-protected storage.

This is not a criticism. It is just a different layer and a different goal than UBI.

---

## 3. The missing layer in the Zephyr stack

The missing piece is a subsystem that sits here:

```text
Application / FS / database / object store
            │
            ▼
        UBI plain / secure
            │
            ▼
      flash_map / flash_area
            │
            ▼
      raw NOR / NAND flash
```

That layer is valuable when the product needs all of these at once:

- a raw-flash abstraction that survives relocation and remapping,
- more than one logical storage region inside one partition,
- wear-leveling across the whole managed region,
- crash-safe metadata updates,
- a reusable substrate for storage engines that do not want to own flash-management logic themselves.

Without such a layer, every higher subsystem that cares about raw flash tends to re-implement some subset of:

- placement policy,
- erase scheduling,
- crash recovery,
- bad-block handling,
- metadata mirroring,
- atomic replacement patterns.

That duplication is exactly what a UBI-like layer avoids.

---

## 4. What plain UBI gives that Zephyr does not currently give as a subsystem

Plain UBI is useful even before secure mode exists.

### 4.1 Logical volumes on raw flash

Plain UBI lets one flash partition host multiple logical volumes with independent logical eraseblock spaces.

That is materially different from "just give each consumer a fixed partition", because UBI manages the whole region as one eraseblock pool and one wear domain.

### 4.2 Virtualization of physical flash

Plain UBI makes the upper layer address **logical eraseblocks**, not physical eraseblocks.

That matters because the upper layer no longer needs to care which physical block currently holds a logical block.

This is the core abstraction that makes:

- copy-on-write replacement,
- garbage collection,
- wear-leveling,
- and future bad-block retirement

practical without leaking raw-flash details upward.

### 4.3 Cross-volume wear-leveling

A plain filesystem or KV engine often wear-levels only within its own allocated area.

UBI wear-levels at the managed-partition level.

That is useful when one product has multiple storage consumers with very different write rates, because the partition can be managed as one flash budget instead of many isolated hot spots.

### 4.4 Metadata redundancy and deterministic recovery

UBI provides reserved metadata areas, mirrored state, and deterministic selection of the newest valid state after reboot.

That is the kind of infrastructure that a higher storage engine benefits from, but usually does not want to implement repeatedly.

### 4.5 Bad-block lifecycle

A raw-flash management layer is the natural place to quarantine bad blocks and keep them out of future allocations.

This is particularly relevant for external NAND and still useful as a clean abstraction boundary for other raw-flash media.

### 4.6 A block-level substrate, not a policy-heavy store

Plain UBI does **not** force file semantics, KV semantics, or log semantics.

That is precisely why it is reusable.

---

## 5. Why UBI is not redundant with NVS, ZMS, FCB, or LittleFS

These technologies overlap only partially.

| Subsystem | Primary abstraction | Best fit | Why it does not replace UBI |
|---|---|---|---|
| Flash map | partition access | low-level flash I/O | no logical remapping, no wear-leveling policy |
| FCB | FIFO log | append-only logs | not a general flash manager |
| NVS | key-value | config/state blobs | fixed KV semantics, not a general LEB/PEB layer |
| ZMS | key-value | more general KV on varied NVM | still a KV store, not a volume manager |
| LittleFS / other FS | files and directories | file storage | filesystem semantics, not a reusable raw-flash substrate |
| Secure Storage / PSA ITS | API-level secure storage service | PSA storage integration | different scope; not a raw-flash volume manager |

So the point of UBI is not "replace all of the above".

The point is:

- make raw flash easier and safer to build on,
- make higher layers smaller because they no longer own flash-management logic,
- give Zephyr a reusable middle layer that is currently missing.

---

## 6. What secure UBI adds on top of plain UBI

Secure UBI keeps the same logical model and adds a secure wrapper around the on-flash objects.

That adds value in places where plain wear-leveling and crash recovery are not enough.

### 6.1 What secure UBI adds

Secure UBI can add:

- confidentiality of on-flash objects,
- authenticity and integrity of those objects,
- binding of data to physical placement and parent metadata,
- authenticated recovery state for future writes,
- key rotation and retirement state,
- exported freshness signals for application policy.

### 6.2 What secure UBI still does not claim

Secure UBI should still document one hard boundary clearly:

- without an external trusted freshness store or other trust anchor, it does **not** provide complete anti-rollback protection by itself.

That is not a weakness of the idea. It is the honest boundary of a software-managed on-flash format.

### 6.3 Why secure UBI is still valuable even with Secure Storage

Secure Storage and secure UBI solve different problems.

Secure Storage gives an API-level storage service.

Secure UBI gives a **storage substrate** that another subsystem can build on. It can protect arbitrary UBI-managed volumes and objects, not only PSA ITS / PS objects.

That makes secure UBI a better fit when the goal is:

- secure raw-flash volumes,
- secure storage below a custom filesystem,
- secure storage below a database engine,
- secure storage for large structured blobs that are not naturally expressed as small ITS objects.

---

## 7. Why plain UBI is a good foundation for a filesystem

A filesystem on raw flash typically needs some combination of:

- block placement,
- atomic replacement,
- garbage collection,
- wear management,
- recovery after interrupted metadata updates.

Plain UBI already solves the flash-management part of that stack.

That gives a filesystem built above UBI several advantages:

- the filesystem can think in terms of **logical blocks**,
- remapping and wear-leveling are below the filesystem boundary,
- bad-block handling is below the filesystem boundary,
- the filesystem can focus on namespace, allocation, caching, and on-disk format.

Zephyr's VFS layer also allows [external filesystems](https://docs.zephyrproject.org/latest/services/storage/file_system/index.html) to be registered. That means UBI does not have to pretend to be a filesystem itself in order to be useful to filesystem work.

---

## 8. Why plain UBI is a good foundation for an LSM-tree database

An LSM-tree engine naturally creates:

- immutable sorted runs,
- manifests / metadata records,
- compaction output,
- write-ahead logs or memtable flush targets.

This maps well to a UBI-style substrate.

### 8.1 Separation of concerns

With UBI below the database:

- the database owns key ordering, compaction policy, and query semantics,
- UBI owns placement on raw flash, wear-leveling, remapping, and recovery of physical media state.

That is a clean split.

### 8.2 Volume-level structure

A database can use separate UBI volumes for distinct roles, for example:

- metadata / manifest,
- WAL or recovery log,
- SSTable or run storage,
- large-value overflow region.

That separation is often easier to reason about than forcing all roles into one raw partition with one custom placement scheme.

### 8.3 Better use of raw flash

An LSM tree already expects data movement during compaction.

UBI is a natural place to absorb the raw-flash consequences of that movement:

- erase scheduling,
- block recycling,
- wear balancing,
- bad-block retirement.

That keeps flash-specific code out of the database core.

### 8.4 Secure UBI for LSM-style storage

If secure UBI is used underneath an LSM tree, the database can inherit:

- authenticated persistence of runs and metadata blocks,
- encrypted-at-rest storage,
- exported freshness signals for application rollback policy.

Again, this does not magically make the whole database transactional or rollback-proof. But it gives the database a much stronger persistence substrate.

---

## 9. Where UBI should not be used

UBI is not the right choice for every storage problem.

It is probably the wrong layer when:

- the medium already has an FTL, such as eMMC or SD,
- the application only needs a small key-value store and NVS or ZMS already fits,
- the application only wants files and does not need a reusable raw-flash manager below the filesystem,
- flash wear is negligible and the simplest solution is good enough.

That boundary is important, because it keeps the case for UBI honest.

---

## 10. Why this is a strong upstream candidate for Zephyr

From a Zephyr-maintainer point of view, plain UBI is interesting because it adds a subsystem that is currently missing, not because it duplicates an existing one.

It would give Zephyr:

- a reusable raw-flash volume manager,
- a wear-leveling substrate that higher layers can share,
- a clean place for bad-block lifecycle and recovery logic,
- a better foundation for future filesystems or databases on raw flash,
- and, with secure UBI, a path toward secure-at-rest raw-flash volumes that integrates naturally with PSA-based key management.

The important upstream argument is therefore:

> plain UBI is already useful on its own, and secure UBI is a natural extension of the same abstraction.

That is a better upstream story than proposing only a crypto feature without first showing why the underlying storage layer belongs in Zephyr.

---

## 11. References

- [Zephyr flash area / flash map API](https://docs.zephyrproject.org/latest/doxygen/html/group__flash__area__api.html)
- [Zephyr FCB](https://docs.zephyrproject.org/latest/services/storage/fcb/fcb.html)
- [Zephyr NVS](https://docs.zephyrproject.org/latest/services/storage/nvs/nvs.html)
- [Zephyr ZMS](https://docs.zephyrproject.org/latest/services/storage/zms/zms.html)
- [Zephyr file systems](https://docs.zephyrproject.org/latest/services/storage/file_system/index.html)
- [Zephyr Secure Storage](https://docs.zephyrproject.org/latest/services/storage/secure_storage/index.html)
- [Zephyr PSA Crypto](https://docs.zephyrproject.org/latest/services/crypto/psa_crypto.html)
- [Zephyr random number generation](https://docs.zephyrproject.org/latest/services/crypto/random/index.html)

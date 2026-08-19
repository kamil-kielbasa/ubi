# Proposal 0001 — `ubi_leb_change()` and `ubi_leb_write_at()`

**Target:** v1.1.0 · **Status:** draft · **Breaks:** public API, on-flash format (plain and secure)

---

## 1. Scope

```
                    ubi_leb_write()                    <- today
                           |
             +-------------+-------------+
             |                           |
      ubi_leb_change()          ubi_leb_write_at()     <- proposed
   whole LEB, fresh PEB,       append in place,
   atomic, closes the LEB      one segment per call
```

Both write **segments**. One on-flash representation, both backends, both
functions. Nothing else changes about volumes, mapping or recovery.

Effects: `data_size` leaves the VID header · the per-read VID lookup disappears ·
`write_at` is all-or-nothing per call · plain gains torn-write detection ·
reported `leb_size` shrinks (§5) · `ubi_leb_map()` is removed.

---

## 2. Decisions

| # | Decision | Reason |
|---|---|---|
| D1 | Both backends frame every write | One model for callers; a frontier is the only guard against silent overwrite (§2.1) |
| D2 | `write_at` is append-only: `offset == frontier` | Holes are not representable in a chain; NAND requires sequential in-block programming |
| D3 | Unaligned `offset` rejected, never realigned | Silent realignment desynchronises the caller's own addressing |
| D4 | `data_size` removed from the VID header | Every segment carries its own length |
| D5 | `enum ubi_volume_type` removed | The write model is a per-LEB property (D6) |
| D6 | VID carries `lnum_type`: `USER_CHANGE` / `USER_APPEND` / `INTERNAL_ANCHOR` | Write-once and authenticated; also retires the anchor's reserved-`lnum` sentinel |
| D7 | `change` closes the LEB | Keeps `change` a replacement, not "replacement plus open tail" |
| D8 | Torn tail closes the LEB; surfaces as `-ENOSPC` | Caller already handles "LEB full"; one recovery path |
| D9 | Segment count bounded per volume, minimum derived from geometry | Bounds derating and the secure counter reservation; the minimum keeps `change` possible on large erase blocks (§5.3) |
| D10 | Secure segments are hash-chained (`prev_tag`); plain segments are not | Chaining detects reordering and insertion — adversary-only events. Plain has no adversary |
| D11 | `ubi_leb_map()` removed | `write_at` auto-maps, `change` allocates its own. Nothing left to reserve |
| D12 | Cached state has three tiers: per-LEB (mandatory), per-volume walk memo (mandatory), per-volume segment index (opt-in). All filled lazily | Appends need the physical frontier anyway; the memo makes sequential reads O(1); only the index scales with `leb_count`, so only it is optional (§7.2) |
| D13 | Relocation is an explicit maintenance op, not a write-path side effect | Relocating on `write_at` moves hot blocks; anti-static levelling needs cold ones (§9.2) |
| D14 | No `LONG_TERM` / `SHORT_TERM` hint | Linux UBI had `dtype` and removed it (~3.7, 2012) |
| D15 | Chunked secure LEB mode deleted | Segments do the same job with caller-chosen boundaries (§4.5) |
| D16 | A torn tail is reported as `UBI_SECURE_EVENT_SEGMENT_TORN`, never as `AUTH_FAILURE` | Power loss is expected; routing it through the auth-failure path would latch read-only on a normal event (§8.6) |

### 2.1 Why plain frames too

On NOR, reprogramming a written cell only clears bits and usually **succeeds** —
`flash_area_write()` returns 0 and the data is silently corrupted. The overwrite
guard must therefore be logical; a logical guard needs a frontier; a frontier
needs framing, because a raw payload may legitimately contain long runs of the
erased value.

---

## 3. API

```c
/* ---- write ------------------------------------------------------------- */

int ubi_leb_change(struct ubi_device *ubi, int vol_id, size_t lnum,
                   const void *buf, size_t len);

int ubi_leb_write_at(struct ubi_device *ubi, int vol_id, size_t lnum,
                     size_t offset, const void *buf, size_t len,
                     struct ubi_leb_info *info /* optional */);

/* ---- read (signature unchanged) ---------------------------------------- */

int ubi_leb_read(struct ubi_device *ubi, int vol_id, size_t lnum,
                 size_t offset, void *buf, size_t len);

/* ---- query ------------------------------------------------------------- */

struct ubi_leb_info {
        bool    mapped;
        bool    closed;    /* no further write_at accepted */
        size_t  frontier;  /* logical bytes stored = offset for the next write_at */
        size_t  max_write; /* largest len the next write_at accepts; 0 when closed */
        uint8_t segments;  /* segments committed so far */
};

int ubi_leb_get_info(struct ubi_device *ubi, int vol_id, size_t lnum,
                     struct ubi_leb_info *info);

/* ---- maintenance ------------------------------------------------------- */

enum ubi_maintenance_op {
        UBI_MAINTENANCE_RECLAIM,   /* erase one dirty PEB */
        UBI_MAINTENANCE_RELOCATE,  /* move one LEB off a low-EC PEB */
        UBI_MAINTENANCE_TORTURE,   /* retry one bad PEB */
};

int ubi_device_maintenance(struct ubi_device *ubi, enum ubi_maintenance_op op);
```

Removed: `ubi_leb_map()`, `ubi_leb_is_mapped()`, `ubi_leb_get_size()`,
`ubi_device_erase_peb()`. `ubi_leb_unmap()` stays — it is the erase primitive.

### 3.1 `frontier` and `max_write` are logical

```
                            frontier = 340
                                  v
   logical   0        100        340
             |---------|----------|
                seg 0     seg 1
             |<-100 B->|<-240 B->|

   physical  |hdr|  100 B  |pad|hdr|  240 B  |pad|  erased
             ^16 B                                ^
             segment overhead                     next segment lands here
```

| Value | Counts headers? | Definition |
|---|---|---|
| `frontier` | **no** | sum of committed payload bytes; exactly what to pass as the next `offset` |
| `max_write` | **yes, already deducted** | `min(65535, physical_left − segment_overhead)`; 0 when closed |

`max_write` answers "does my next record fit" — the only question a caller asks.
It is deliberately not "total bytes still writable", which depends on how the
caller will split the rest and is therefore unusable.

### 3.2 `ubi_leb_write_at()` contract

| Condition | Result |
|---|---|
| `offset != frontier` | `-EINVAL` |
| `offset % write_block_size != 0` | `-EINVAL` |
| `len == 0` | `-EINVAL` |
| `len > 65535` | `-EFBIG` |
| LEB not mapped | auto-mapped as `USER_APPEND` |
| LEB is `USER_CHANGE` | `-EPERM` |
| full, budget spent, or torn tail | `-ENOSPC`, decided before any flash mutation |
| flash error mid-write | `-EIO`; committed segments untouched; LEB closed |

### 3.3 `ubi_leb_read()` behaviour changes

| Situation | Today | Proposed |
|---|---|---|
| LEB not mapped | `-ENOENT` | erased value, zero flash I/O, returns 0 |
| range past the frontier | `-EINVAL` vs `data_size` | erased value, zero flash I/O, returns 0 |
| range past `leb_size` | `-EINVAL` | `-EINVAL` |
| per read | VID read + CRC | none |

### 3.4 Volume configuration

```c
struct ubi_volume_config {
        char    name[UBI_VOLUME_NAME_MAX_LEN];
        size_t  leb_count;
        uint8_t leb_max_segments;   /* min_segments()..255 */
        bool    segment_cache;      /* build the per-volume segment index (§7.2) */
};
```

`type` is gone. `leb_max_segments` is per-volume, not device-wide, because the
derating is severe in secure mode on small erase blocks (§5.2). `segment_cache`
is per-volume because its RAM cost scales with `leb_count` — affordable on a
small journal volume, not on a large store.

### 3.5 Device info

```c
struct ubi_device_info {
        ...
        size_t free_peb_count;
        size_t dirty_peb_count;   /* -> UBI_MAINTENANCE_RECLAIM  */
        size_t bad_peb_count;     /* -> UBI_MAINTENANCE_TORTURE  */
        size_t reloc_leb_count;   /* -> UBI_MAINTENANCE_RELOCATE   (new) */
        size_t leb_raw_size;      /* was leb_size; underated payload area */
        ...
};
```

Each counter names the op that drains it. Policy stays with the caller:

```
   ubi_device_get_info(&info)
        |
        +-- info.dirty_peb_count > 0 --> ubi_device_maintenance(RECLAIM)
        +-- info.reloc_leb_count > 0 --> ubi_device_maintenance(RELOCATE)
        +-- info.bad_peb_count   > 0 --> ubi_device_maintenance(TORTURE)
```

`ubi_device_maintenance()` performs **one item** and returns 0, or `-ENOENT`
when there is nothing to do for that op. The library imposes no priority and
runs no unbounded loop.

---

## 4. On-flash format

### 4.1 Invariant

> Every byte of user data lives inside a segment. There is no unframed data.

This replaces `data_size` for `write_at` **and** for `change`. `change` is
`fresh PEB + segment(s) + VID`; a `USER_CHANGE` LEB is normally one segment, so
its length costs one header read.

### 4.2 PEB layout

```
  plain
  0      16       48                                                 ebs
  +------+--------+-------------+------+-------------+------+---------+
  |  EC  |  VID   | seg 0       | pad  | seg 1       | pad  | erased  |
  +------+--------+-------------+------+-------------+------+---------+

  secure
  0      64      160                                                 ebs
  +------+--------+-------------+------+-------------+------+---------+
  |  EC  |  VID   | seg 0       | pad  | seg 1       | pad  | erased  |
  +------+--------+-------------+------+-------------+------+---------+
```

### 4.3 Segment

```
  plain segment                     16 B + payload
  +--------------------------+---------------------+
  | seg_hdr                  | payload             |
  +--------------------------+---------------------+
   magic ver flags len off idx crc32

  secure segment                    48 B + payload
  +--------------------------+---------------------+-------+
  | prefix32                 | ciphertext          | tag16 |
  +--------------------------+---------------------+-------+
   magic ver dom kv flags salt ctr len off idx
```

```c
#define UBI_SEG_HDR_MAGIC (0x55424927)

struct ubi_seg_hdr {                    /* 16 B = WRITE_BLOCK_SIZE_ALIGNMENT */
        uint32_t magic;
        uint8_t  version;
        uint8_t  flags;             /* reserved */
        uint16_t payload_len;       /* 1..65535 */
        uint8_t  logical_offset[3]; /* be24 — frontier before this segment */
        uint8_t  segment_index;     /* 0..leb_max_segments-1 */
        uint32_t crc;               /* crc32(hdr[0x00..0x0B] || payload) */
};
```

Widths follow the limits: 65535 is the AES-CCM `q = 2` payload cap, adopted in
plain for symmetry; 3 offset bytes cover 16 MiB; 1 index byte caps
`leb_max_segments` at 255.

Secure reuses 6 of `prefix32`'s 12 reserved bytes — prefix size unchanged:

```c
        /* was reserved[12] */
        uint8_t payload_len[2];     /* be16 */
        uint8_t logical_offset[3];  /* be24 */
        uint8_t segment_index;      /* u8  */
        uint8_t reserved[6];
```

### 4.4 Write order

Header first, payload second. A self-describing header survives a torn payload,
which is what makes §6 decidable.

| Operation | Order | Commit point | `lnum_type` |
|---|---|---|---|
| `ubi_leb_write_at()`, first call | VID, then segment 0 | segment 0 | `USER_APPEND` |
| `ubi_leb_write_at()`, later calls | segment *k* | segment *k* | — |
| `ubi_leb_change()` | segments, then VID | VID | `USER_CHANGE` |

Four observable states, no ambiguity:

```
  no VID + no data      -> free
  no VID + data         -> torn change, old mapping survives
  VID USER_APPEND       -> append log, walk the chain
  VID USER_CHANGE       -> complete change
```

### 4.5 Chunked mode is replaced (D15)

Both exist only because CCM `q = 2` caps one invocation at 65535 bytes.

| | chunked | segments |
|---|---|---|
| piece size | fixed, Kconfig | chosen per call |
| per-piece cost | 16 B (tag) | 48 B (prefix + tag) |
| piece count | derived from `data_size` | walked |
| 64 KiB `change` | 16 × 16 B = 256 B (4 KiB chunks) | 2 × 48 B = 96 B |
| partial read | touched chunks | touched segments |
| append | impossible | native |

Segments are cheaper for large writes because they are not capped at a small
constant, and they keep the partial-read property.
`CONFIG_UBI_SECURE_LEB_CHUNKED` and `CONFIG_UBI_SECURE_LEB_CHUNK_SIZE` are
removed; `ubi_leb_change()` with `len > 65535` emits `ceil(len / 65535)`
segments. Zero-length secure LEB records also disappear — a mapped LEB with no
segments is simply frontier 0.

### 4.6 VID header

The 4 bytes at `0x18` that held `data_size`:

```
0x18  1  lnum_type      0 = USER_CHANGE, 1 = USER_APPEND, 2 = INTERNAL_ANCHOR
0x19  1  max_segments   the budget in force for this mapping
0x1A  2  reserved
```

`max_segments` is stored per mapping because the secure counter reservation
(§8.1) is sized from it; recovery must use the value actually reserved.

### 4.7 Padding — existing bug

The write-block tail is padded with **zeros** today
(`lib/src/plain/ubi_plain_io_data.c`). Under `write_at` that burns the tail of a
write block to `0x00` and breaks "unwritten flash reads as the erased value".
Must use the hardware-reported erased value.

> Same function, separate latent bug: `align_buf` is 16 B but
> `write_block_size` bytes are written from it — an overread when
> `write_block_size > 16`.

---

## 5. Capacity

### 5.1 Derating

```
  peb_payload_area
  |<--------------------------------------------------------------->|
  | seg0 | seg1 | seg2 | ...                 |    reserved tail      |
                                             |<--------------------->|
                                        max_segments * segment_overhead

  segment_overhead(plain)  = 16 + (write_block_size - 1)
  segment_overhead(secure) = 48 + (write_block_size - 1)

  leb_size = peb_payload_area - max_segments * segment_overhead
```

`leb_size` from `ubi_volume_get_info()` is the **contract** — fixed and always
safe. `max_write` from `ubi_leb_get_info()` is the **exact** remainder.

### 5.2 Cost, `write_block_size = 4`

| `max_segments` | 4 KiB plain | loss | 4 KiB secure | loss | 64 KiB secure | loss |
|---|---|---|---|---|---|---|
| 1 | 4029 | 1.6 % | 3885 | 5.2 % | 65325 | 0.3 % |
| 8 | 3896 | 4.9 % | 3528 | 13.9 % | 64968 | 0.9 % |
| 16 | 3744 | 8.6 % | 3120 | 23.8 % | 64560 | 1.5 % |
| 32 | 3440 | 16.0 % | 2304 | 43.8 % | 63744 | 2.7 % |
| 64 | 2832 | 30.9 % | 672 | 83.6 % | 62112 | 5.2 % |

Negligible on large erase blocks, brutal on 4 KiB in secure. Suggested default
16, with "drop to 8 or lower for secure volumes on 4 KiB erase blocks".

### 5.3 Minimum from geometry (D9)

A segment holds at most 65535 payload bytes, so `n` segments must satisfy
`n * 65535 >= peb_payload_area - n * segment_overhead`:

```
  min_segments = ceil(peb_payload_area / (65535 + segment_overhead))
```

| erase block | plain | secure |
|---|---|---|
| 4 KiB | 1 | 1 |
| 64 KiB | 1 | 1 |
| 128 KiB | 2 | 2 |
| 256 KiB | 4 | 4 |

`ubi_volume_create()` rejects `leb_max_segments < min_segments` with `-EINVAL`,
so `change` is always able to fill a LEB.

---

## 6. Crash handling

```
  ubi_leb_write_at()

  |------ header ------|--------- payload (+ tag) ---------|
  ^                    ^                                   ^
  A                    B                                   C
```

| Crash | Tail on flash | Walk verdict | LEB |
|---|---|---|---|
| before A | erased | end of chain | **writable** |
| inside A | partial header | structural check fails | **closed** |
| B .. C | header ok, payload short | CRC / tag fails | **closed** |
| after C | complete | verifies | **writable** |

Every failure case gives the same result: the half-written segment is invisible,
the frontier is exactly where it was, and the LEB stops accepting appends. That
is what makes `write_at` all-or-nothing per call.

Distinguishing "before A" is free: the walk reads those 16/32 bytes anyway to
learn that the chain ended, and `ubi_buf_is_erased()` runs on a buffer already
in RAM. A valid header can never be all-erased, because of the magic.

**No resume after a torn segment.** A torn payload leaves a valid header stating
its length, so UBI could skip it — but the skipped range would be a hole in the
logical stream (D2). Closing costs at most one partially used erase block per
power loss.

**No retry inside `write_at`.** `flash_write_with_retry()` retries at the same
offset. Inside `write_at` that is a guaranteed write-once violation and a NAND
NOP violation. A failed write is terminal for that LEB.

Crash during `change` (data, no VID) and during the initial VID write (no VID,
no data) are classified by the existing attach scan, unchanged.

---

## 7. Walk and verification

### 7.1 Walk

```
  pos = data_start ; acc = 0 ; idx = 0

  loop:
      read header @ pos                          16 B plain / 32 B secure
      |
      +-- all erased?          -> frontier = acc ; open   ; stop
      |
      +-- structural fail?     -> frontier = acc ; closed ; stop
      |     magic, version, 1 <= payload_len <= 65535,
      |     logical_offset == acc, segment_index == idx,
      |     align_up(hdr + payload_len) fits
      |
      +-- next header erased?  -> this is the last segment
      |        verify CRC / tag
      |        +-- fail        -> frontier = acc ; closed ; stop
      |        +-- ok          -> frontier = acc + payload_len ; open ; stop
      |
      acc += payload_len ; idx++ ; pos += align_up(hdr + payload_len)
```

Only the **last** segment is verified during the walk — that is all the frontier
needs. Intermediate segments are verified when they are read (§7.3).

In secure this makes frontier recovery **one AEAD invocation**: `logical_offset`
is inside `prefix32`, hence inside the AAD, so the last segment's tag
authenticates its physical offset, index, length and logical offset at once.

### 7.2 Three tiers of cached state (D12)

```
  tier 1   per mapped LEB, mandatory      ~12 B      appends O(1)
  tier 2   per volume, mandatory          ~16 B      sequential reads O(1) amortised
  tier 3   per volume, opt-in             leb_count * max_seg * 2 B   random reads O(1)
```

All three are **pure accelerators over the walk and never authoritative**.
Dropping any of them changes speed, not results, so none needs persistence or an
invalidation protocol beyond "reset on unmap". All are filled **lazily**, on
first touch — never at attach, which would mean walking every mapped LEB before
the device becomes usable.

#### Tier 1 — per-LEB state

An append has to know where the next header physically goes, so this is not an
optimisation:

```c
struct ubi_leb_state {          /* ~12 B per mapped LEB, in the EBA node */
        uint32_t frontier;      /* logical bytes committed */
        uint32_t phys_next;     /* where the next segment header goes */
        uint8_t  segments;      /* segments committed */
        uint8_t  flags;         /* closed */
};
```

With tier 1 alone, locating an arbitrary logical offset costs header reads:

```
  append                         -> 0 header reads (phys_next cached)
  read at offset X               -> (k + 1) header reads, k = covering segment
  read from the start            -> 1
  worst case (X in last segment) -> max_segments
  full sequential scan           -> O(n²/2)   <- each read restarts the walk
```

That last line is what tier 2 removes.

#### Tier 2 — per-volume walk memo

One resume point per volume:

```c
struct ubi_walk_memo {          /* ~16 B per volume */
        size_t   lnum;
        uint32_t logical_offset; /* start of the memoised segment */
        uint32_t phys_offset;
        uint8_t  segment_index;
};
```

A lookup in the same LEB at an offset at or past the memo resumes the walk from
the memo instead of from segment 0. A sequential scan therefore advances one
segment per read:

```
  without memo   read seg 0: 1 read   seg 1: 2 reads   seg 2: 3 reads ...
  with memo      read seg 0: 1 read   seg 1: 1 read    seg 2: 1 read  ...
```

No eviction policy — a miss simply overwrites the memo and restarts from
segment 0. This covers the dominant journal pattern (append at the head, scan
from the start) and is why tier 3 is rarely needed.

#### Tier 3 — per-volume segment cache

Enabled by `ubi_volume_config.segment_cache`. When the pointer is non-NULL the
volume has one and every lookup is pure RAM, including random access.

The cache stores **only payload lengths**. Both offset series are prefix sums of
that one array, so nothing else has to be kept:

```
  logical_offset[i] = sum( len[j]                    for j < i )
  phys_offset[i]    = sum( align_up(hdr + len[j])    for j < i ) + data_start
```

```
  vol->seg_cache          uint16_t, leb_count * leb_max_segments entries

  +--------- leb 0 ---------+--------- leb 1 ---------+----------
  | len0 | len1 | .. | lenN | len0 | len1 | .. | lenN | ...
  +------+------+----+------+------+------+----+------+----------
     2 B    2 B         2 B

  lookup(lnum, X):
      acc = 0 ; phys = data_start
      for i in 0 .. state[lnum].segments - 1:
          len = cache[lnum][i]
          if X < acc + len:  return (phys + hdr_size, X - acc, len)
          acc  += len
          phys += align_up(hdr_size + len)
```

A linear scan over at most 255 `uint16_t` in RAM is cheaper than one flash
transaction, so no ordered structure is needed.

```
  RAM = leb_count * leb_max_segments * 2 B

  leb_count   max_segments   RAM
       8           16        256 B
      32           16        1 KiB
     128           16        4 KiB
     512           16       16 KiB
    2046           16       64 KiB      <- do not enable
```

It is a full table for the whole volume, not a set of evictable slots: the flag
is all-or-nothing. RAM therefore scales with `leb_count`, which is exactly why
the flag is per-volume — a small journal volume can afford it, a large store
cannot and should rely on tiers 1 and 2.

Allocation goes through `ubi_mem`. Under `CONFIG_UBI_MEM_BACKEND_HEAP` it is a
plain allocation; under the default static backend it is carved out of a single
`CONFIG_UBI_SEGMENT_CACHE_SIZE` byte pool, and `ubi_volume_create()` returns
`-ENOMEM` when the pool cannot satisfy the request. That keeps the static memory
model intact: the ceiling is still fixed at build time.

### 7.3 What is verified, and when

| Check | Covers | Plain | Secure |
|---|---|---|---|
| structural | magic, version, `payload_len` range, `logical_offset == acc`, `segment_index == idx`, fits | every header read | every header read |
| integrity | `hdr ‖ payload` (CRC32) / AAD ‖ ciphertext (tag) | last segment on walk; every segment on read | same |
| chaining | previous segment's tag in AAD | — | every verification |

The structural check is ~60 bits of constraint (4-byte magic + an exact 3-byte
offset + an exact index), so it cannot accept garbage by accident. It exists to
make the walk cheap, not to replace the CRC.

Plain does **not** chain, for two reasons:

1. **It buys nothing.** A chain detects reordering and mid-chain insertion.
   Neither happens by accident — segments sit at fixed physical positions and
   `logical_offset` + `segment_index` already catch a misparse. Both are
   adversary-only events; plain has no adversary in its threat model.
2. **It is not free.** Verifying segment *i* would need `crc_{i-1}` from the
   previous header — one extra read per verification.

Secure has no such choice: authenticating on every read *is* the security claim.

---

## 8. Secure specifics

### 8.1 AEAD counter continuity

```
  today          write data -> write VID(counter = actual usage)     OK
  with write_at  write VID  -> append, append, append ...            counter stale
                              ^ cannot be updated afterwards
  fix            write VID(counter = base + max_segments) -> append ...
```

`vid_secure_meta.leb_write_counter` is the next unused counter for
`{key_version, volume_id}`. Under `write_at` the segments come after the VID
record, so a stale recovered floor would mean counter reuse — and CCM counter
reuse means CTR keystream reuse.

```c
vid_meta.leb_write_counter    = base + max_segments;
vid_meta.leb_total_auth_bytes = base_bytes + leb_size
                              + max_segments * UBI_SECURE_LEB_AAD_SIZE;
```

- The 48-bit counter space makes the over-reservation irrelevant.
- The invariant *"the VID record is authoritative for counters"* is untouched,
  so §9.5 of the on-flash spec needs no change.
- Attach still does not trust data-area prefixes.
- **Hidden anchors, the last-writable-witness check and the emergency-free-PEB
  reserve keep working unmodified** — they read VID metadata, which is still
  complete.

Price: a LEB that received 2 appends burns budget as if it received
`max_segments`. `leb_max_segments` therefore controls capacity **and** key
rotation rate; the Kconfig help must say so. A journal doing 16 small appends
instead of one `change` reaches `KEY_ROTATE_SOON` 16× sooner, so the budgets
must be rescaled.

### 8.2 Hidden anchor via `lnum_type`

Today an anchor is identified by a reserved `lnum` outside the user range. With
`lnum_type = INTERNAL_ANCHOR` (D6) the sentinel disappears: the anchor is
identified by an explicit authenticated field, the attach scan classifies by
type instead of comparing against a magic value, and "user-visible `leb_count`
excludes the anchor" becomes explicit rather than derived. The anchor also stops
needing a zero-length LEB record — a VID with no segments is enough.

### 8.3 AAD

Today's 74 B, minus `data_size` (4 B, gone), plus `prev_tag` (16 B) = **86 B**.
`payload_len`, `logical_offset` and `segment_index` come along inside
`prefix32`.

```
  prefix32(32) || peb_index(4) || flash_offset(8) || ec(8) || parent_ec_kv(1)
               || vol_id(4) || lnum(4) || sqnum(8) || parent_vid_kv(1)
               || prev_tag(16)
```

`prev_tag` for segment 0 is the **EC header's tag** — not the VID's, because
`change` writes the VID last and that would be circular.

### 8.4 Truncation

| Attack | Before (`data_size` in VID) | After |
|---|---|---|
| modify a segment | detected | detected |
| reorder segments | detected | detected |
| insert mid-chain | detected | detected (`prev_tag`) |
| **truncate the tail** | **detected** — exact length authenticated in VID | **not detected** — caller gets an authentic *prefix* |

A deliberate regression; it belongs in the normative spec in those words. The
only closure is exporting `{lnum, segments, last_tag}` in the freshness
descriptor, so the application can pin journal state — the same mechanism
already used for `device_revision`.

### 8.5 Read granularity

A read authenticates the segment(s) covering the range: 4 bytes out of a 4 KiB
segment costs an AEAD pass over 4 KiB.

> The segment size the caller chooses is also its read granularity.

### 8.6 Torn tail is an event, not a crypto failure (D16)

A torn tail fails its tag, so without special handling it would reach the
application as `UBI_SECURE_EVENT_AUTH_FAILURE` — an event many applications
answer with `ENTER_READ_ONLY`. Power loss during `write_at` is a normal
occurrence, so it gets its own type:

```c
UBI_SECURE_EVENT_SEGMENT_TORN = 10,   /*!< Trailing LEB segment failed
                                           authentication. The LEB is closed
                                           for further appends; committed
                                           segments are unaffected. */

/* in the ubi_secure_event union */
struct {
        uint32_t peb_index;
        uint32_t volume_id;
        uint32_t lnum;
        uint8_t  segment_index;   /* the segment that failed */
} segment;                        /*!< SEGMENT_TORN. */
```

Normative rules:

- the library does **not** latch read-only on this event, whatever the verdict
  says about other events;
- it is emitted once, when the walk closes the LEB — not on every subsequent
  read;
- UBI cannot distinguish a power-loss tear from a deliberately corrupted tail.
  It reports the fact and its location; the policy decision is the
  application's. An application that never loses power unexpectedly should treat
  this event as an attack signal.

Plain reports the same condition through the log only — it has no event channel.

---

## 9. Wear-levelling

### 9.1 What `write_at` breaks

A full LEB pinned to a PEB is normal — a journal is a *sequence* of LEBs, and
`ubi_leb_unmap()` returns the PEB to the pool.

The real problem: there is no wear-levelling worker. The only levelling
mechanism is churn, and `write_at` removes it.

```
  change:    map -> write -> unmap -> erase -> free      EC rises, block rotates
  write_at:  map -> append -> append -> ... (long)       EC frozen

  allocation is rb_get_min(free_pool)  ->  the healthiest block gets pinned
                                           by data that never moves
```

### 9.2 Why relocation is an explicit op

| | relocate on `write_at` | `UBI_MAINTENANCE_RELOCATE` |
|---|---|---|
| latency | a 64-byte append becomes a full-LEB rewrite (+ re-encrypt in secure) | bounded, off the write path |
| which blocks move | the LEB being appended to — **hot**, and about to be recycled anyway | the coldest mapped LEB |
| static data | never reached: a LEB written once is never a trigger | reached — the trigger is EC spread, not activity |
| secure budget | re-encrypts hot data repeatedly | amortised |

Anti-static levelling exists to move data that *nothing is writing*; a trigger
that fires on writes cannot see it.

`reloc_leb_count` counts mapped LEBs whose PEB satisfies
`max_ec - ec > CONFIG_UBI_WL_EC_THRESHOLD`. Relocation is internally a `change`
with the same `lnum` and a higher `sqnum`; in secure it needs a full re-encrypt,
because the AAD binds `peb_index` and `flash_offset`.

### 9.3 Spread the allocation

`rb_get_min(free_pool)` returns the single least-worn PEB. Fix: pick at random
among the `K` least-worn (`K = 8`, or the lower quartile). One line, no API
change, reduces how often relocation is needed.

---

## 10. Kconfig

| Symbol | Range | Default | Effect |
|---|---|---|---|
| `CONFIG_UBI_LEB_MAX_SEGMENTS` | `min_segments()`–255 | 16 | Default segment budget: capacity derating and the secure counter reservation |
| `CONFIG_UBI_WL_EC_THRESHOLD` | — | 512 | EC spread above which a LEB is counted in `reloc_leb_count` |
| `CONFIG_UBI_SEGMENT_CACHE_SIZE` | — | 0 | Bytes reserved for per-volume segment caches under the static memory backend. 0 disables the feature |

Removed: `CONFIG_UBI_SECURE_LEB_CHUNKED`, `CONFIG_UBI_SECURE_LEB_CHUNK_SIZE`.

RAM cost (§7.2): ~12 B per mapped LEB and ~16 B per volume, both mandatory, plus
`leb_count * leb_max_segments * 2 B` for each volume that opts into tier 3.

---

## 11. Migration

| Element | Today | Proposed | Class |
|---|---|---|---|
| `ubi_leb_write()` | exists | → `ubi_leb_change()` | rename |
| `ubi_leb_write_at()` | — | new | addition |
| `ubi_leb_map()` | exists | removed | removal |
| `ubi_leb_is_mapped()`, `ubi_leb_get_size()` | exist | → `ubi_leb_get_info()` | consolidation |
| `ubi_device_erase_peb()` | exists | → `ubi_device_maintenance(op)` | rename + split |
| `ubi_device_info` | — | `+ reloc_leb_count`, `leb_size` → `leb_raw_size` | addition + rename |
| `ubi_volume_get_info()` | — | reports the derated `leb_size` | addition |
| `ubi_leb_read()` unmapped / past frontier | `-ENOENT` / `-EINVAL` | erased value, returns 0 | semantics |
| `enum ubi_volume_type`, `config.type` | exist | removed → `leb_max_segments`, `segment_cache` | removal |
| `enum ubi_secure_event_type` | 10 values | `+ UBI_SECURE_EVENT_SEGMENT_TORN` | addition |
| `ubi_vid_hdr.data_size` | exists | → `lnum_type` + `max_segments` | **format** |
| `struct ubi_seg_hdr` | — | new, 16 B | **format** |
| `prefix32.reserved[12]` | reserve | `payload_len`, `logical_offset`, `segment_index` | **format** |
| LEB AAD | 74 B | 86 B (−`data_size`, +`prev_tag`) | **format** |
| `vid_meta.leb_write_counter` | actual usage | reservation of `max_segments` | format semantics |
| chunked LEB mode | Kconfig-gated | removed | **format** |
| zero-length secure LEB record | required for anchors | removed | **format** |
| anchor `lnum` sentinel | reserved value | `lnum_type = INTERNAL_ANCHOR` | **format** |
| write-block padding | zeros | erased value | bug fix |
| `align_buf` with `wbs > 16` | overread | fixed | bug fix |
| retry inside `write_at` | — | forbidden | new rule |

`UBI_VID_HDR_VERSION` and `UBI_SECURE_WRAPPER_VERSION` both bump. No in-place
migration.

---

## 12. Order

Steps 1–3 are independent of the format discussion.

| # | Step | Format | API |
|---|---|---|---|
| 1 | Pad with the erased value; fix `align_buf` | no | no |
| 2 | Cache `data_size` in the EBA node — removes the VID read from the read path | no | no |
| 3 | `ubi_leb_read()` on an unmapped LEB returns the erased value | no | minor |
| 4 | `ubi_leb_write()` → `ubi_leb_change()` | no | yes |
| 5 | `struct ubi_seg_hdr`, walk, per-LEB state; `change` emits segments | **yes** | no |
| 6 | `ubi_leb_write_at()` plain; closing rules; no retry; drop `ubi_leb_map()` | no | yes |
| 7 | `leb_max_segments` + geometry minimum; derating; `ubi_leb_get_info()` | no | yes |
| 8 | Remove `enum ubi_volume_type`; VID `lnum_type` + `max_segments` | **yes** | yes |
| 9 | `prefix32` fields; secure segments; delete chunked mode and zero-length records | **yes** | no |
| 10 | `prev_tag` in AAD; counter reservation; anchor by type | **yes** | no |
| 11 | `ubi_leb_write_at()` secure | no | no |
| 12 | `UBI_SECURE_EVENT_SEGMENT_TORN`; export `segments` / `last_tag` in the freshness descriptor | no | yes |
| 13 | `reloc_leb_count`; `ubi_device_maintenance(op)`; allocation spreading | no | yes |
| 14 | Tier 3 per-volume segment cache | no | yes |

The consumer can move to the plain backend after step 7; secure catches up at 11.
Tiers 1 and 2 of §7.2 land with steps 5–6. Step 14 is a pure accelerator and can
be dropped or deferred without affecting anything else.

---

## 13. Open questions

None outstanding. Everything raised during review is folded into §2 as a
decision.

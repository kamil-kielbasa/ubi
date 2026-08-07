# Journaling

**Status:** proposal — not implemented.
**Audience:** implementers and reviewers.
**Scope:** a per-volume write-back journal for UBI — on-flash format, runtime model, secure-mode
impact, API changes, and rollout.

**Prerequisites:** {doc}`/architecture/plain_architecture` and {doc}`/architecture/secure_overview`.

---

## 1. Problem and solution

Every `ubi_leb_write()` rewrites a whole LEB into a freshly allocated PEB. Writing 16 bytes costs
the same as writing 4 KiB, so updating a small record 100 times burns 100 PEBs.

The fix is the one flash filesystems already use: append small writes as self-describing records
into a dedicated LEB, and materialise them into their targets only when that LEB fills up or the
application asks. `N` small writes then cost `ceil(N / records_per_journal_LEB)` PEBs.

| System | Small in-LEB writes | Journal |
|--------|---------------------|---------|
| Linux UBI | `ubi_leb_write(desc, lnum, buf, offset, len)` — append inside a LEB, rewriting an already-written region is forbidden | none — UBI has no journal |
| UBIFS | writes go to journal *buds*; `wbuf` batches sub-`min_io_size` writes in RAM | log + buds + commit; data **stays** in the journal, commit only updates the index |
| NVS / FCB | append-only records, no random-access volumes | the log *is* the storage |
| **This proposal** | `ubi_leb_write()` gains an `offset` and **may** rewrite a written region | per-volume write-back journal; records are replayed into target LEBs and the journal is erased |

We take the UBIFS bud idea and make it write-back rather than log-structured, so that reads of a
non-journaled LEB stay exactly as fast as today and the journal never becomes a permanent lookup
cost.

## 2. Model

Journaling is a per-volume feature. A volume without journal LEBs behaves exactly as today.

| Term | Meaning |
|------|---------|
| Journal LEB | A PEB owned by a volume whose payload area is an append-only record log |
| Journal | The `journal_leb_count` journal LEBs of one volume, ordered by `sqnum` |
| Record | `(target lnum, target offset, payload)` — one `ubi_leb_write()` call |
| Live record | Present in the RAM index; not yet materialised into its target LEB |
| Flush | Replay every live record into its target LEB, then commit and retire the journal |

```text
ubi_leb_write() ──> append record ──> open journal LEB
                                          │
                    journal full ─────────┤
                    ubi_volume_sync() ────┼──> flush: replay into target LEBs,
                    ubi_leb_change() ─────┤         write COMMIT, retire journal
                    volume resize/remove ─┘
```

## 3. LEB type in the VID header

A journal PEB is not a LEB with an odd number — it is a PEB whose payload area follows different
parsing rules. That is a statement about *type*, so it belongs in a type field.

`struct ubi_vid_hdr` has three padding bytes after `version`. One becomes `leb_type`:

| Value | Name | `lnum` means | Payload area |
|-------|------|--------------|--------------|
| 0 | `UBI_LEB_TYPE_DATA` | index into the volume, `[0, leb_count)` | flat payload of `data_size` bytes |
| 1 | `UBI_LEB_TYPE_JOURNAL` | journal slot, `[0, journal_leb_count)` | append-only record log; `data_size` is 0 |
| 2 | `UBI_LEB_TYPE_ANCHOR` | unused, always 0 | empty (secure continuity anchor) |

`sizeof(struct ubi_vid_hdr)` stays 32 bytes and `hdr_crc` already covers the padding, so the VID
record size does not change. `UBI_VID_HDR_VERSION` goes `1 -> 2`.

**This replaces the anchor sentinel.** `UBI_SECURE_INTERNAL_ANCHOR_LNUM (UINT32_MAX)` and its
warning comment — *"This value must never collide with user-visible lnum range"* — are deleted,
together with the point check in the attach scan. Anchors are recognised by `leb_type` alone. No
legacy recognition path is kept: the on-flash format has no deployed images.

`leb_type` is bound into the secure LEB AAD, so a record cannot be reinterpreted under a different
LEB type even if every other AAD field is preserved. `UBI_SECURE_LEB_AAD_SIZE` goes `74 -> 75` and
`UBI_SECURE_LEB_CHUNK_AAD_SIZE` goes `78 -> 79`; both `BUILD_ASSERT`s are updated. The extra
authenticated byte per record is charged to `leb_total_auth_bytes` like any other AAD byte.

`(vol_id, lnum)` is now unique only *within a type*, so the scan must route on type before touching
any table:

```text
vid_hdr.leb_type ─┬─ DATA    ──> vol->eba_tbl          (key: lnum)
                  ├─ JOURNAL ──> vol->journal_slots    (key: slot)
                  ├─ ANCHOR  ──> vol->anchor_pnum
                  └─ unknown ──> dirty pool + LOG_WRN
```

`scan_resolve_dup()` loses its hardcoded `vol->eba_tbl` and takes the destination table as a
parameter; the "higher `sqnum` wins" rule is unchanged.

`data_size = 0` on a journal VID has a useful side effect: `ubi_leb_read()` rejects
`offset + len > data_size`, so a journal PEB cannot be read through the public API even if a
caller guesses its slot number.

## 4. On-flash format

```text
Plain journal PEB                       Secure journal PEB
0x0000 +---------------------+          0x0000 +---------------------+
       | EC header      16 B |                 | secure EC      64 B |
0x0010 +---------------------+          0x0040 +---------------------+
       | VID header     32 B |                 | secure VID     96 B |
       | leb_type=JOURNAL    |                 | leb_type=JOURNAL    |
0x0030 +---------------------+          0x00A0 +---------------------+
       | journal record 0    |                 | journal record 0    |
       | journal record 1    |                 | journal record 1    |
       | ...                 |                 | ...                 |
       | erased              |                 | erased              |
       +---------------------+                 +---------------------+
```

The EC and VID headers are unchanged. What follows them is new.

### 4.1 Journal record header

`struct ubi_jrec_hdr` — 32 bytes, identical in both modes, aligned to
`WRITE_BLOCK_SIZE_ALIGNMENT`:

| Offset | Field | Size | Notes |
|--------|-------|------|-------|
| 0 | `magic` | 4 | `0x55424927` |
| 4 | `version` | 1 | 1 |
| 5 | `type` | 1 | `UBI_JREC_TYPE_DATA` (0) or `UBI_JREC_TYPE_COMMIT` (1) |
| 6 | `reserved0` | 2 | |
| 8 | `lnum` | 4 | target LEB; 0 for `COMMIT` |
| 12 | `offset` | 4 | target offset inside the LEB; 0 for `COMMIT` |
| 16 | `len` | 4 | payload length; 0 for `COMMIT` |
| 20 | `data_crc` | 4 | CRC32 of the payload — plain mode only, 0 in secure |
| 24 | `reserved1` | 4 | |
| 28 | `hdr_crc` | 4 | CRC32 of this header — plain mode only, 0 in secure |

### 4.2 Record framing

**Plain.** A record is `ubi_jrec_hdr || payload`, padded to `write_block_size`. Write order is
`PAYLOAD -> HEADER`, mirroring the `DATA -> VID` rule: the header is the commit point. A record is
live only when the magic and `hdr_crc` are valid **and** `data_crc` matches the payload.

**Secure.** A record is `prefix16 || ciphertext(ubi_jrec_hdr || payload) || tag16`. Both CRC fields
are zero; the tag is the integrity mechanism. AAD is the LEB AAD of § 3 with `flash_offset` set to
the record's offset within the PEB. The record header is **inside** the ciphertext, so the target
`lnum` and `offset` are encrypted: raw-flash access reveals how many records exist and how large
they are, but not which LEB or which byte range they touch.

### 4.3 Why journal records use a 16-byte prefix

The 32-byte `prefix32` used by EC, VID and whole-LEB records carries a 4-byte magic and 12 reserved
bytes that a journal record does not need: the containing VID already declares `leb_type = JOURNAL`,
so the parser knows what to expect before it reads the prefix, and "is there a record here" is
answered by the existing erased-region check rather than by a magic.

| Field | Size | Why it must stay |
|-------|------|------------------|
| `wrapper_version` | 1 | Format evolution |
| `domain` | 1 | `UBI_SECURE_DOMAIN_LEB` — journal records reuse the whole-LEB domain and key (§ 6); the byte is still a nonce input |
| `key_version` | 1 | Selects `IKM[v]` |
| `flags` | 1 | Reserved |
| `salt` | 6 | Nonce input |
| `counter` | 6 | Nonce input |

That is exactly 16 bytes, so secure per-record overhead is `16 + 16 = 32` bytes instead of 48. At a
16-byte payload that is 80 bytes per record instead of 96 — 20 % more records per journal LEB.
Whole-LEB records keep `prefix32` unchanged.

## 5. Runtime

### 5.1 A journal of several LEBs

A volume reserves `journal_leb_count` journal LEBs, numbered `0 .. journal_leb_count - 1` in the
`lnum` field of their VID headers. Exactly one is **open** at any time and receives appends; the
rest are either full or unused.

```text
volume 3, journal_leb_count = 3

slot 0  PEB 11  sqnum 41  [rec rec rec rec] full
slot 1  PEB 27  sqnum 47  [rec rec rec rec] full
slot 2  PEB 19  sqnum 52  [rec rec .. .. ] open   <- appends land here
```

Global record order is `(journal LEB sqnum, record offset)`. Because `sqnum` is allocated when the
journal LEB's VID is written, slot numbers say nothing about age — the scan sorts by `sqnum`.

When the open LEB has no room for the next record, the next unused slot is opened. When no unused
slot remains, the journal is flushed, which retires every slot and opens a fresh one. Reserving
more than one journal LEB therefore trades capacity for a lower flush frequency; it changes no other
rule.

### 5.2 RAM index

Reads must not walk the journal on flash. The index holds descriptors only — never payloads:

| Field | Size | Notes |
|-------|------|-------|
| `lnum` | 4 | target LEB |
| `target_off` | 4 | target offset |
| `len` | 2 | payload length |
| `jrec_blk` | 2 | record offset in the journal LEB, in `write_block_size` units |

12 bytes per record, in a flat array segmented by journal slot, so the journal PEB number is implied
by the segment and is not stored. The array is sized once at attach from
`journal_leb_count * CONFIG_UBI_JOURNAL_MAX_RECORDS_PER_LEB`, and
`UBI_JOURNAL_MAX_RECORDS_PER_LEB` carries a Kconfig `range`: the integrator picks how many records a
journal LEB may hold, and that choice fixes both the index RAM and the secure counter reservation of
§ 6.

### 5.3 Write

`ubi_leb_write(ubi, vol_id, lnum, offset, buf, len)` on a volume with no journal is a direct
in-place write into the mapped PEB: the LEB must be mapped and the target region must still be
erased, otherwise `-EINVAL`. That is Linux UBI semantics.

With a journal, the call appends one record. Let `stride = ROUND_UP(record_size,
write_block_size)`:

| Condition | Action |
|-----------|--------|
| `offset + len > leb_size` | `-EINVAL` |
| `stride >` usable capacity of an empty journal LEB | `-ENOSPC` — the payload can never fit; use `ubi_leb_change()` |
| `stride <=` free space in the open journal LEB | Append |
| `stride >` free space, an unused slot exists | Open the next slot, append there |
| `stride >` free space, no unused slot | Flush, then append into the fresh journal LEB |

The `-ENOSPC` row is a static property of the configuration —
`stride > erase_block_size - journal_payload_offset - commit_stride` — so it is checked before any
flash access and a caller cannot trigger a pointless flush by asking for the impossible.

**`ubi_leb_change()` flushes the journal first.** The two operations write the same LEB by different
means, and `sqnum` cannot order them: it orders PEBs, and every record inside a journal LEB shares
that LEB's `sqnum`. Take a journal LEB opened at `sqnum 10`:

```text
write(5, "AAAA")    -> record r1 in the journal      (journal sqnum 10)
change(5, "ZZZZ")   -> new PEB for lnum 5, sqnum 11
write(5, "BBBB")    -> record r2 in the journal      (journal sqnum 10)
crash
```

After a reboot nothing on flash tells `r1` and `r2` apart: they sit in the same journal LEB, and
that LEB's `sqnum` is the only timestamp replay has. The information that would settle it — that the
change happened between them — was never recorded. Any decision taken at journal-LEB granularity is
therefore wrong for one of the two: drop the LEB's records for `lnum 5` and `r2` is lost, keep them
and `r1` overwrites the change. Flushing first removes the ambiguity, because the journal is empty
when the change runs.

The flush rebuilds `lnum 5` itself as well, even though the change is about to overwrite it. That is
deliberate: `r1` is an acknowledged durable write, so an interrupted `ubi_leb_change()` must leave
either *old content plus `r1`* or *the new content*, never a third state in which `r1` silently
disappeared. One extra PEB is the price of that guarantee.

So on a journaled volume `ubi_leb_change()` is expensive, and a workload that interleaves it with
small writes flushes constantly — keep whole-LEB traffic on a volume without a journal, or batch it.
When the journal holds no live records the flush is a no-op and the call costs what it costs today.

`ubi_leb_map()`, `ubi_leb_unmap()`, `ubi_volume_resize()`, `ubi_volume_remove()` and
`ubi_device_deinit()` flush for the same reason; § 5.6 explains why that list has to be exhaustive.

### 5.4 Read

```text
ubi_leb_read(lnum=7, offset=0, len=32)

base LEB 7        aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa   1 read (+1 AEAD in secure)
record #2  (7,4)      bbbb                           overlay
record #5  (7,20)                     cccccc         overlay
                  --------------------------------
result            aaaabbbbaaaaaaaaaaaaccccccaaaaaa
```

Overlays are applied in append order, so the newest write to a byte wins without any extra
bookkeeping. Each overlay costs one small flash read, plus one AEAD operation in secure mode. In
single-tag secure mode the base read authenticates the **entire** LEB payload regardless of the
requested slice, which makes overlays comparatively cheap and chunked mode the right pairing.

### 5.5 Flush

The RAM index already knows which LEB every live record targets — reads need exactly that. Flush
starts from that list and rebuilds one target at a time.

```text
journal LEBs : 4, 9, 12          J_max = max sqnum over the journal LEBs holding live records
targets      : 0, 2, 3, 5, 7, 11, 14, 18, 21      (from the RAM index)

for each target T, ascending:
        if T is mapped and sqnum(T) > J_max:
                skip T                            # already rebuilt, see 5.6 rule 3

        content := current content of T            # empty if T is unmapped
        for each journal LEB J, ascending sqnum:
                for each record of J targeting T, in append order:
                        apply the record onto content
        write content into a fresh PEB and swap the mapping

for each journal LEB J:
        append COMMIT to J

retire every journal LEB to the dirty pool, open a fresh one
```

Rebuilding a target is one whole-LEB write, so it goes through the existing
`leb_prepare_new_mapping()` + `leb_commit_mapping_swap()` path and gets a fresh `sqnum`. "Current
content of T" is a logical view: § 5.7 forbids holding a whole LEB, so the implementation slides a
window over the target instead of buffering it.

### 5.6 Crash behaviour

Flush is a loop, not an atomic operation, so it can be interrupted anywhere. Three rules make an
interrupted flush safe to simply run again from the top.

**Rule 1 — a crash before the COMMITs means: start over.** The journal is untouched, the RAM index
rebuilds identically from flash, and rebuilding a target is idempotent because it is always
`base content + the same records in the same order`. Re-running the whole flush is always correct.

**Rule 2 — COMMIT is per journal LEB.** Each journal LEB reserves its last record slot for its own
`COMMIT`, which states *every record in this LEB has been materialised*. A crash while writing them
leaves some LEBs marked and some not:

```text
LEB 4   [ rec rec rec COMMIT ]     retired
LEB 9   [ rec rec rec COMMIT ]     retired
LEB 12  [ rec rec rec        ]     <- crash here; only this LEB survives attach
```

A half-marked journal can only occur *after* every target was rebuilt, because the `COMMIT`s come
last. So the next flush re-examines LEB 12's records, finds every one of their targets already ahead
of the journal by rule 3, rebuilds nothing, and just writes the missing `COMMIT`. Here rule 3 buys
speed rather than correctness: LEB 12 holds the newest records for those targets, so re-applying
them would reproduce the same bytes anyway — rule 3 only saves the pointless rebuild.

**Rule 3 — a target ahead of the journal is already done.** Every target rebuilt by a flush gets a
`sqnum` higher than any journal LEB in that flush, because those journal LEBs were opened earlier.
So:

```text
T is mapped and sqnum(T) > J_max   =>   T was rebuilt by an interrupted flush   =>   skip it
```

The converse cannot happen: while a journal LEB is live, a target only gets a new `sqnum` from a
flush of that journal. Every other path that would remap it — `ubi_leb_change()`, `ubi_leb_map()`,
`ubi_leb_unmap()`, `ubi_volume_resize()`, `ubi_volume_remove()`, `ubi_device_deinit()` — flushes
first (§ 5.3), which retires the journal. An unmapped target has no `sqnum` and is always rebuilt.

The check is cheap and runs *before* the rebuild, so a restarted flush touches only the targets that
were not finished. A crash after seven of ten targets costs three rebuilds, not ten.

Rule 2 is also what retires the journal. `ubi_leb_unmap()` writes nothing to flash — it moves the
PEB to the dirty pool in RAM, and the PEB keeps a valid EC and VID header until
`ubi_device_erase_peb()` physically erases it. A data LEB survives that because a newer PEB claiming
the same `lnum` wins on `sqnum`; a retired journal LEB has no successor, so without `COMMIT` the
attach scan would keep finding it and re-indexing it forever. Rule 3 cannot cover this case on its
own: a record whose target was legitimately unmapped after the flush has no PEB to compare against.

### 5.7 Memory requirements

These are constraints on the implementation, not observations:

1. The RAM index is allocated once, at attach, and sized
   `journal_leb_count * CONFIG_UBI_JOURNAL_MAX_RECORDS_PER_LEB * 12` bytes per journaled volume.
   Nothing on the write path allocates.
2. Flush must not buffer a whole LEB. The static allocator exposes **one** scratch block, sized
   `max(UBI_DEV_HDR_SIZE + MAX_VOLUMES * UBI_VOL_HDR_SIZE, 2 * CHUNK_SIZE + 16)`, which is unrelated
   to `leb_size`.
3. Flush must not hold that scratch block across a call that allocates scratch itself. Every secure
   read and write does.
4. Flush therefore streams: `write_block_size` at a time in plain mode, one chunk at a time in
   secure mode.
5. `CONFIG_UBI_JOURNAL` depends on `CONFIG_UBI_SECURE_LEB_CHUNKED` when `CONFIG_UBI_SECURE` is
   enabled, because requirement 4 cannot be met in single-tag mode.

## 6. Secure mode

Journal records are ordinary LEB-domain records: same `UBI_SECURE_DOMAIN_LEB`, same per-volume key
`K_leb[key_version][vol_id]`, same counter space as whole-LEB writes. Introducing a journal-specific
domain would create a second counter space that needs its own floor recovery, its own budget base
and its own continuity anchor; reusing the LEB domain means the hidden per-volume anchor already
guarantees continuity across reclaim, `unmap` and reboot, with nothing new to maintain.

What the journal does need is a way to allocate counters from that shared ratchet. The per-volume
floor is recovered at attach from `vid_secure_meta` in each PEB's VID header, but a journal LEB
writes its VID **once**, at open time, and then appends `N` records — each one an AEAD invocation.
Recovering the floor from the VID alone would leave it `N` counters too low after a reboot and reuse
nonces.

So opening a journal LEB reserves a span of the ratchet up front. Its VID records
`leb_write_counter = counter_base + CONFIG_UBI_JOURNAL_MAX_RECORDS_PER_LEB` and the matching
worst-case `leb_total_auth_bytes`; its records — including its `COMMIT` — consume that span and
nothing else. Counters are a one-way ratchet, so an under-filled journal LEB simply burns the
remainder, and attach recovers the correct floor without reading a single record. One
`ubi_secure_budget_leb_pre()` call at open, covering the whole span, gives back-pressure before the
burst rather than halfway through it.

Flush draws from the same ratchet. Rebuilding a target is an ordinary secure whole-LEB write: it
consumes LEB-domain counters, advances `vol->cached_leb_write_counter`, and stores the new floor in
the rebuilt target's own `vid_secure_meta`. Because the journal LEB's reservation was already
recorded when it was opened, those counters land after the reserved span — the two never overlap,
and nonce uniqueness holds without any extra bookkeeping.

The reservation is exactly the Kconfig value, so the integrator sizes the nonce budget directly. For
`UBI_JOURNAL_MAX_RECORDS_PER_LEB = 64` and a 4 KiB PEB:

| | Reserved per journal LEB | Journal LEBs until the budget is spent |
|---|---|---|
| Counters | 64 | 15 625 (`LEB_WRITE_BUDGET` = 10^6) |
| Auth bytes | `64 * 75 + 3936` ≈ 8.7 KB | 11 500 (`10^8` — not the binding limit) |

Lowering the Kconfig value tightens the reservation and raises the number of journal LEBs a key
version can serve; raising it reduces flush frequency at the cost of counters.

| Attack | Defence |
|--------|---------|
| Delete a record from the middle | AAD binds `flash_offset`; the scan stops at the resulting hole, so the tail is dropped rather than shifted |
| Reorder records | Same — a record only authenticates at the offset it was written to |
| Move a record to another PEB | AAD binds `peb_index`, `ec` and the parent VID's `sqnum` |
| Move a journal LEB to another volume | The LEB key is derived per `vol_id` |
| Replay a retired journal LEB | `COMMIT` is authenticated like any other record |
| Reinterpret a data record as a journal record | AAD binds `leb_type`, and the VID that declares the type carries its own tag |

`(vol_id, lnum)` no longer being unique is not exploitable: a data record and a journal record may
share both values, but their AAD still differs in `leb_type`, `peb_index`, `flash_offset`, `ec` and
`sqnum`.

## 7. Attach and recovery

```text
for each data PEB:
        read EC, read VID
        switch vid.leb_type:
                DATA     -> vol->eba_tbl[vid.lnum]       (higher sqnum wins)
                JOURNAL  -> vol->journal_slots[vid.lnum] (higher sqnum wins)
                ANCHOR   -> vol->anchor_pnum
                unknown  -> dirty pool, LOG_WRN

for each volume, for each journal LEB J in ascending sqnum:
        offset := payload start
        while true:
                read the record header at offset       # AEAD-authenticate it in secure mode
                if erased or invalid  -> break         # end of log, torn tail is cut here
                if type == COMMIT     -> drop every record indexed from J; break
                add (lnum, offset, len) to the RAM index
                offset += stride

        if J contributed no live records:
                retire J to the dirty pool

if any live records remain:
        flush (§ 5.5) — rule 3 skips targets already rebuilt
```

A journal LEB whose VID write was interrupted has both the VID region and the payload area erased,
so the existing free/uncommitted classification puts it back in the free pool. That is why the
inverted `VID -> records` commit order is safe.

| Crash point | On-flash state | After attach |
|-------------|----------------|--------------|
| Journal VID write | VID and payload erased | PEB is free |
| After VID, before first record | Valid VID, no records | Empty journal LEB, ready for use |
| Record payload write | Partial payload, no header | Scan stops there; record ignored |
| Record header write | Payload complete, header torn | CRC (or tag) fails; scan stops; record ignored |
| Mid-flush, some targets rebuilt | Journal intact, those targets carry a higher `sqnum` | Flush re-runs; rule 3 skips the rebuilt ones |
| Between two `COMMIT` writes | Some journal LEBs marked, some not | Only the unmarked LEBs are replayed |
| After all `COMMIT`s, before retire | Every journal LEB marked | All records dropped; journal LEBs retired |

## 8. API and Kconfig

| Today | Proposed |
|-------|----------|
| `ubi_leb_write(ubi, vol_id, lnum, buf, len)` | `ubi_leb_change(ubi, vol_id, lnum, buf, len)` |
| — | `ubi_leb_write(ubi, vol_id, lnum, offset, buf, len)` |
| — | `ubi_volume_sync(ubi, vol_id)` |
| `struct ubi_volume_config { name, type, leb_count }` | `+ size_t journal_leb_count` (0 disables journaling) |

The names follow Linux UBI: `ubi_leb_change()` is the atomic whole-LEB update, `ubi_leb_write()`
writes at an offset, and `ubi_volume_sync()` mirrors `ubi_flush()` scoped to one volume. One
deliberate divergence: our `ubi_leb_write()` **may** rewrite an already-written region when the
volume has a journal. That is the feature.

`journal_leb_count` persists in `ubi_vol_hdr.padding_2[0]`; the remaining 8 bytes stay reserved, so
`UBI_VOL_HDR_SIZE` is unchanged. A volume reserves `leb_count + journal_leb_count` PEBs.

| Symbol | Range | Default | Purpose |
|--------|-------|---------|---------|
| `UBI_JOURNAL` | — | `n` | Enable journaling. `depends on UBI_SECURE_LEB_CHUNKED` if `UBI_SECURE` |
| `UBI_JOURNAL_MAX_LEBS_PER_VOLUME` | 1–8 | 2 | Upper bound validated at volume create |
| `UBI_JOURNAL_MAX_RECORDS_PER_LEB` | 8–256 | 64 | Index RAM **and** the secure counter reservation |
| `UBI_JOURNAL_AUTO_FLUSH_PCT` | 50–100 | 90 | Flush threshold |

## 9. Cost model

Record stride is the header plus the payload, rounded up to `write_block_size`. For a 4 KiB PEB the
payload area of a journal LEB is `4096 - 48 = 4048` bytes in plain mode and `4096 - 160 = 3936` in
secure mode; with `UBI_JOURNAL_MAX_RECORDS_PER_LEB = 64` and one slot reserved for `COMMIT`:

| | Framing at a 16-byte payload | Stride | Fit | Capped | Usable |
|---|---|---|---|---|---|
| Plain | `jrec_hdr(32) + payload(16)` | 48 B | `4048 / 48` = 84 | 64 | 63 |
| Secure | `prefix16(16) + jrec_hdr(32) + payload(16) + tag(16)` | 80 B | `3936 / 80` = 49 | 64 | 48 |

100 writes of 16 bytes to one LEB, `journal_leb_count = 1`. "PEBs consumed" counts allocations from
the free pool, which is what wear-levelling pays for:

| | Sequence | PEBs consumed | vs today |
|---|---|---|---|
| Today | 100 whole-LEB writes | 100 | 1x |
| Plain | 63 records, flush, 37 records | 2 journal + 1 target = 3 | **33x** |
| Secure | 48 records, flush, 48 records, flush, 4 records | 3 journal + 2 target = 5 | **20x** |

| Resource | Cost |
|----------|------|
| RAM | `journal_leb_count * MAX_RECORDS_PER_LEB * 12` — 768 B for one journal LEB of 64 records |
| Capacity | `journal_leb_count` PEBs per volume unavailable to user data |
| Attach | One extra PEB read per journal LEB, plus one AEAD per record in secure mode |
| Flush | No additional scratch — streaming reuses the existing block |

Journaling pays off while at least two records fit in a journal LEB. Above `journal_capacity / 2` a
record degenerates into a whole-LEB write with extra overhead — use `ubi_leb_change()` instead, but
note that on a journaled volume it costs a full flush (§ 5.3).

## 10. Rollout, tests, non-goals

| Phase | Content | Depends on |
|-------|---------|------------|
| 1 | API split: `ubi_leb_change()` + `ubi_leb_write()` with `offset` as a direct in-place write. No journal | — |
| 2 | `leb_type` in `ubi_vid_hdr`, `UBI_VID_HDR_VERSION` 1->2, `leb_type` in the LEB AAD, type-routed scan, parameterised `scan_resolve_dup()`, removal of `UBI_SECURE_INTERNAL_ANCHOR_LNUM` | — |
| 3 | Plain journal: volume config, slot management, record format, `COMMIT`, RAM index, merged read, streaming flush, scan | 1, 2 |
| 4 | Secure journal + hardening: `prefix16`, counter-span reservation on the existing LEB domain, budgets; fault injection, stress, forensic scan, doc updates | 3 |

Phases 1 and 2 are independently useful and independently testable.

Tests (Zephyr `ztest`, `bash scripts/run_tests.sh native_sim {plain|secure|chunked}`):

- **Functional** — append, merged read, flush, `ubi_volume_sync()`, newest-wins on overlapping
  records, slot rotation with `journal_leb_count > 1`, every row of the § 5.3 table,
  `ubi_leb_change()` forces a flush.
- **Ordering** — `write(L)`, `change(L)`, `write(L)`, crash: after reboot the LEB holds the changed
  content with the second write applied and no trace of the first.
- **LEB type** — a data LEB and a journal LEB both numbered 0 in one volume survive a reboot;
  unknown `leb_type` lands in the dirty pool without failing attach; anchors resolve by `leb_type`
  only, with a regression check that no `UINT32_MAX` lnum sentinel remains.
- **Recovery** — every row of the § 7 table; a flush of ten targets interrupted after seven replays
  only the remaining three; a retired-but-unerased journal LEB is **not** replayed after reboot; a
  repeated replay is idempotent.
- **Secure** — counter monotonicity across reboot, rejection of tampered tag / offset / prefix,
  rejection of a VID with a swapped `leb_type`, budget exhaustion at reservation time, forensic scan
  finding no plaintext in journal records.
- **Format regression** — `sizeof(struct ubi_vid_hdr) == 32`, `sizeof(struct ubi_jrec_hdr) == 32`,
  `UBI_SECURE_LEB_AAD_SIZE == 75`, whole-LEB secure record sizes unchanged.
- **Memory** — no slab leaks, no scratch `-ENOSPC` during flush under the static allocator.

Non-goals: multi-LEB atomic transactions, a device-wide journal, migration of existing volumes, and
any change to wear-levelling or bad-block handling.

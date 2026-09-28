# Energy Ledger durable store format

Format name: `EnergyLedger.SegmentStore`
Format version: **1** (implemented by Energy Ledger 1.0.0)
Byte order on disk: **little-endian**, written and read byte by byte, so the
format does not depend on host endianness. Every fixed structure carries an
explicit endian marker that is validated on read.

This document is normative for the bytes this implementation writes and the
bytes it accepts. Anything not described here is rejected.

---

## 1. Canonical units and arithmetic

* The canonical energy unit is the **joule**, stored as a signed 64-bit integer.
  No authoritative quantity is ever a floating-point value.
* Wire units (J, kJ, MJ, GJ, mJ, uJ, Wh, kWh, MWh, GWh, TWh) are converted to
  joules through exact rational factors. A declared quantity that is not a whole
  number of joules is **refused** (`inexact-unit-conversion`); it is never
  truncated or rounded.
* Active power (watts) and energy (joules) are distinct types. Converting power
  to energy requires an explicit interval duration and is checked; an
  integration that is not exact is refused (`integration-inexact`).
* Every authoritative sum, difference, product and unit conversion is checked.
  Overflow is refused (`arithmetic-overflow`); it is never wrapped and never
  silently clamped.

## 2. Store layout

```
<store>/
  LEDGER.STORE            immutable store descriptor
  MANIFEST.A              manifest generation slot A (odd generations)
  MANIFEST.B              manifest generation slot B (even generations)
  MANIFEST.STG            staging copy of the manifest being published
  WRITER.LEASE            writer lease and rollback floor
  SEGMENTS/
    SEG-<20 digits>.ELS   one segment file per segment
```

A directory rather than a file is used where a file is required, a reparse
point is used as the store root, or a path contains traversal, device names,
reserved characters, control characters, malformed UTF-8 or overlong input: all
of these are refused before any byte is read or written.

## 3. Fixed structures

### 3.1 Common header (24 bytes, all structures)

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | magic |
| 8 | 4 | format version (1) |
| 12 | 4 | header byte count |
| 16 | 4 | endian marker `0x01020304` |
| 20 | 4 | flags, must be zero |

Magic values: descriptor `ELLEDGR1`, manifest `ELMANIF1`, segment
`ELSEG001`, lease `ELLEASE1`, seal/verification trailer `ELTRL001`,
record frame `ELR1`.

### 3.2 Descriptor — `LEDGER.STORE`, 96 bytes

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 64 | common header, store UUID (24..40), canonical unit code (40..44, 1 = joule), reserved (44..64, zero) |
| 64 | 32 | SHA-256 of bytes [0, 64) |

### 3.3 Manifest — `MANIFEST.A` / `MANIFEST.B` / `MANIFEST.STG`

Header 224 bytes, then `segment_count` × 64 bytes of sealed segment table,
then a 48-byte trailer.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 24 | common header |
| 24 | 16 | store UUID |
| 40 | 16 | ledger incarnation |
| 56 | 8 | generation (commit ordinal, strictly increasing) |
| 64 | 8 | head sequence |
| 72 | 8 | retained entry count |
| 80 | 8 | retirement floor sequence |
| 88 | 8 | sequence at the retirement floor (equals the floor) |
| 96 | 32 | chain head |
| 128 | 32 | chain anchor at the retirement floor |
| 160 | 8 | writer epoch that published this generation |
| 168 | 8 | sealed segment count |
| 176 | 8 | active segment identifier |
| 184 | 8 | active segment first sequence |
| 192 | 8 | active segment committed byte count |
| 200 | 8 | active segment entry count |
| 208 | 16 | reserved, must be zero |
| 224 | 64·n | sealed segment table |
| 224+64·n | 48 | trailer: magic (8), CRC-32 (4), reserved (4, zero), SHA-256 (32) |

Sealed segment table entry (64 bytes): identifier (8), first sequence (8),
entry count (8), committed byte count (8), SHA-256 of the committed digest
region (32).

CRC-32 and SHA-256 cover exactly bytes [0, 224 + 64·n).

### 3.4 Segment — `SEG-<20 digits>.ELS`

Header 64 bytes, then framed records, then — once the segment is sealed — a
72-byte seal trailer.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 24 | common header |
| 24 | 16 | store UUID |
| 40 | 8 | segment identifier |
| 48 | 8 | declared first sequence |
| 56 | 8 | reserved, must be zero |

Record frame:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic `ELR1` |
| 4 | 4 | body length (76 + content length, 76 .. 65536) |
| 8 | 4 | CRC-32 of the body |
| 12 | 8 | sequence |
| 20 | 32 | previous chain value |
| 52 | 32 | entry hash |
| 84 | 4 | content length (0 .. 4096) |
| 88 | · | canonical content bytes |

Seal trailer (72 bytes): magic (8), record count (8), last sequence (8),
reserved (16, zero), SHA-256 over the committed digest region (32). The digest
region is the segment header plus all records, that is
`committed_bytes − 72`. The active segment has no seal trailer and therefore no
seal digest; its integrity is established by per-record hashes and the chain.

### 3.5 Writer lease — `WRITER.LEASE`, 128 bytes

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 24 | common header |
| 24 | 16 | store UUID |
| 40 | 16 | writer identity |
| 56 | 8 | writer epoch |
| 64 | 8 | operating-system process id |
| 72 | 8 | rollback floor generation |
| 80 | 8 | informational write time (never part of any integrity claim) |
| 88 | 8 | reserved, must be zero |
| 96 | 32 | SHA-256 of bytes [0, 96) |

Writer identity is derived, not random: `writer_id = SHA-256(domain
"EnergyLedger.WriterId.v1" || store_uuid || epoch)` truncated to 16 bytes. A
replayed or stale lease therefore produces a different identity than the live
session that holds authority.

## 4. Canonical entry content

Encoded little-endian, length-prefixed, and bounded. Version byte 1.

| Order | Field |
| --- | --- |
| 1 | version (1) |
| 2 | entry kind |
| 3 | observation quality |
| 4 | time basis |
| 5 | authority tier (0..15) |
| 6 | declared unit |
| 7..8 | reserved, must be zero |
| 9 | canonical quantity, signed 64-bit joules |
| 10 | declared value, signed 64-bit |
| 11 | source revision (u64) |
| 12 | source generation (u64) |
| 13 | source epoch (u64) |
| 14 | target sequence (u64, zero when unused) |
| 15 | interval start ticks (i64) |
| 16 | interval end ticks (i64) |
| 17 | observed-at ticks (i64) |
| 18 | annotation count (u16, 0..16) |
| 19 | object, generation, interval, clock domain, source family, source instance, authority (each u16 length + bytes, 1..128 printable ASCII) |
| 20 | annotations: key (u16 + bytes, 1..64) and value (u16 + bytes, 0..128), strictly increasing by key |

Enumeration values:

* entry kind: 1 delivered, 2 consumed, 3 wasted-lost, 4 unclassified,
  5 reservation-commit, 6 curtailed, 7 correction, 8 supersession,
  9 compensating
* observation quality: 0 unknown, 1 verified, 2 estimated, 3 synthetic,
  4 rejected
* time basis: 0 unspecified, 1 monotonic ticks, 2 UTC unix milliseconds,
  3 UTC unix seconds, 4 wall clock minutes, 5 meter register ticks,
  6 interval index
* declared unit: 1 J, 2 kJ, 3 MJ, 4 GJ, 5 mJ, 6 uJ, 7 Wh, 8 kWh, 9 MWh,
  10 GWh, 11 TWh

Decoding is strict: trailing bytes, a non-zero reserved byte, an unknown
version, an unknown enumerator, a length beyond the documented bound, a
duplicate annotation key, unsorted annotations or a declared value that does not
equal the canonical quantity are all refused. Every accepted byte string decodes
to exactly one content and re-encodes to exactly the same bytes.

Time sentinel: an unknown tick value is `INT64_MIN`. It is deliberately not
zero, because zero is a legitimate instant.

## 5. Digests and the integrity chain

Domain-separated hashes, each `SHA-256(domain || 0x00 || payload)`:

| Value | Domain | Payload |
| --- | --- | --- |
| content digest | `EnergyLedger.EntryContent.v1` | canonical content bytes |
| event identity | `EnergyLedger.EventId.v1` | canonical content bytes, first 16 bytes of the digest |
| entry hash | `EnergyLedger.EntryHash.v1` | sequence (u64 LE) then content bytes |
| chain value | `EnergyLedger.Chain.v1` | previous chain value then entry hash |
| segment digest | `EnergyLedger.SegmentDigest.v1` | committed segment digest region |

The chain starts at zero for the first entry of an incarnation, or at the
retirement anchor after retention. Because the sequence and the previous chain
value are inside the hashed payload, omission, duplication and reordering of
committed records are detectable, not only corruption.

## 6. Validation precedence

`validate_content` applies a fixed ladder; the first failing step determines the
reported status code, so the same invalid request always produces the same
primary error:

1. entry kind enumerator
2. observation quality enumerator
3. time basis enumerator
4. declared unit enumerator
5. object reference
6. generation reference
7. interval reference
8. clock domain reference
9. source family reference
10. source instance reference
11. authority reference
12. interval/time consistency: an unspecified basis requires unknown interval
    bounds and an unknown observation time; a declared basis requires known
    bounds with start ≤ end
13. quantity sign: kinds that assert a quantity must not be negative
14. declared quantity consistency with the canonical quantity
15. target: required for correction, supersession and compensating entries, and
    forbidden otherwise
16. annotations: count, key and value bounds, uniqueness and ordering

`append` then applies, in order: idempotent replay by request id, idempotent
replay by event identity, expected incarnation, expected head, expected
generation, entry-count and byte ceilings, adjustment target existence and key
match, supersession authority, writer-lease fence, and finally the commit
protocol.

## 7. Commit protocol and recovery

Publication of one append:

1. validate, canonicalize, and compute the event identity and entry hash;
2. fence the writer lease (re-read the lease through the writer's own locked
   handle);
3. rotate the segment when the active segment is full, sealing it with a
   trailer;
4. write the framed record, flush it durably, and read it back and compare;
5. write the new manifest to `MANIFEST.STG`, flush it, and verify the read-back;
6. write the new manifest to its generation slot, **flush it durably** — this
   flush is the commit point — and verify the read-back;
7. adopt the new head in memory and record idempotency metadata.

An append is acknowledged only after step 6 completes. If any step before the
commit point fails, the staged bytes are truncated back to the committed length
and the in-memory state is restored from a snapshot taken before staging.

Recovery on open:

1. verify the descriptor;
2. read and verify both manifest slots, and adopt the highest valid generation;
3. refuse when the committed generation is below the recorded rollback floor, or
   when the committed manifest was published by an epoch newer than the lease;
4. with writer authority, raise the epoch, rewrite and re-verify the lease, and
   raise the floor to the committed generation;
5. verify every committed record, rebuild the index and walk the integrity chain;
6. report uncommitted trailing residue in the active segment and unreferenced
   segment files, and discard them in a writer session.

A manifest is only usable when it is internally consistent: magic, version,
endian marker, zero flags, length matching its segment table, trailer CRC and
digest, strictly increasing segment identifiers, contiguous sequence ranges,
entry counts that agree with the segment table, a head sequence that agrees
with the accounting, and a chain head that agrees with the entry count. A store
never stitches together partial generations: exactly one whole verified
generation is adopted, or the open is refused.

## 8. Retention

`compact` retires whole sealed segments that lie entirely below the requested
retention point, never the active segment. The new manifest records the
retirement floor, the sequence at that floor and the chain anchor of the last
retired entry. Because the chain is a hash chain, that anchor cryptographically
commits to every retired entry, so retention preserves verifiable semantics:
verification after compaction still starts from an anchor that depends on all
prior history. Retired entries can no longer be inspected and are reported as
`entry-retired`.

## 9. Threat model

The integrity model detects, and the implementation refuses or reports:
accidental corruption, single-bit mutation anywhere in any store file including
headers, trailers, reserved fields and the lease, torn writes, truncation at any
byte boundary, record omission, record duplication, record reordering, segment
reordering, segment duplication, segment substitution from another store,
manifest corruption, manifest rollback to an older generation, an unreadable or
destroyed lease, and uncommitted residue left by an interrupted append.

It does **not** defend against an adversary who can rewrite every byte of the
store, including the descriptor, both manifest slots and the lease, and who can
execute the compaction path: there is no external root of trust, no secret key
and no remote attestation. The rollback floor in the lease is a monotonicity
fence against accidental or naive rollback, not a defence against an attacker
who controls the whole directory. The store is trusted by configuration: its
directory is chosen by the operator, and reparse points are rejected where a
regular file is required.

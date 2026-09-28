# Energy Ledger

Energy Ledger is the durable, provenance-preserving accounting ledger for
facility electrical energy. It records what sources asserted about a facility
object over an interval, commits those assertions to an append-only
integrity-chained sequence, and answers questions about totals, provenance,
reconciliation and residual energy without ever rewriting history.

Repository 24 of 72 in the Data Center Control Plane (DCCP) programme,
Tranche 3 — Electrical Infrastructure Control.

* Language: C++20, standard library only, no third-party dependencies.
* Library, administration CLI, examples, benchmark, test suite and a versioned
  CMake package.
* Validated on Windows x64 with MSVC 19.44 (Visual Studio 2022 Build Tools),
  CMake 3.20+, Ninja. POSIX code paths exist and are structurally complete but
  are **not** verified in this environment.

## Systems boundary

The ledger owns:

* durable accounting identity for facility electrical energy: ledger
  incarnation, committed sequence, and the immutable entries themselves;
* the accounting categories **delivered**, **consumed**, **curtailed**,
  **wasted/lost**, **unclassified** and **reservation/commitment**;
* provenance: which source family, instance, generation, revision and epoch
  asserted a value, in which clock domain and time basis, with which
  observation quality;
* conflict, unknown and residual reporting;
* deterministic interval, object and generation rollups;
* correction, supersession and compensating records that preserve the entries
  they modify;
* integrity: framing, digests, an integrity chain, crash-safe publication,
  recovery, verification and retention.

The ledger does **not** own, model or decide:

* instantaneous power capacity, headroom or reservation policy;
* electrical topology, feeds, breakers or transfer authority;
* control policy, load-shedding selection or any actuation;
* device or adapter command authority;
* billing, tariffs or market settlement;
* telemetry collection.

It consumes observations and commitments as **evidence** and records accepted
accounting events. It never infers a physical measurement that was not observed,
never promotes a command or an acknowledgement into a physical outcome, and
never silently converts power into energy without an explicit interval and the
checked integration contract described in [docs/FORMAT.md](docs/FORMAT.md).

### Core question

> What electrical energy was authoritatively recorded for this facility object
> and interval, under which source/provenance/generation, how does it reconcile
> across accounting categories, and which residuals or conflicts remain
> unexplained?

## Model

### Categories and domains

| Kind | Domain | Meaning |
| --- | --- | --- |
| delivered | measured flow | energy delivered at the object boundary |
| consumed | measured flow | energy consumed by loads under the object |
| wasted-lost | measured flow | delivered energy that was not consumed |
| unclassified | measured flow | delivered energy the source could not classify |
| reservation-commit | commitment | energy reserved or committed for the object |
| curtailed | commitment | energy made unavailable by a curtailment decision |
| correction | adjustment | signed delta applied to an earlier entry |
| compensating | adjustment | signed delta that offsets an earlier entry |
| supersession | adjustment | replacement value for an earlier entry |

Measured-flow accounting and commitment accounting are **separate identities**.
They are reported side by side and are never merged into one balance.

```
measured flow : delivered = consumed + wasted-lost + unclassified-recorded + residual
commitment    : commitment-residual = committed - delivered - curtailed
```

The residual is computed, reported and preserved. It is never assigned to a
category, never hidden, and never silently zeroed. A residual of zero is a
measurement outcome, not an invariant.

### Observation, authority and resolution

Observations of the same accounting key (object, generation, interval, clock
domain, category) compete. Resolution is deterministic and never picks an
arbitrary winner:

1. an explicit `supersession` displaces its target when its authority tier is at
   least as high as every entry it displaces; a weaker supersession is refused
   at append time;
2. otherwise the highest authority tier wins;
3. then the highest source epoch (a re-provisioned device retires older epochs);
4. then the highest source revision;
5. entries that lose are `shadowed` — retained, reported, excluded from totals;
6. equal rank with different observations is a **conflict**: no winner is chosen,
   no quantity is summed, and the candidate range is reported;
7. entries of equal rank that agree on quantity, quality and unit are reported
   as an authoritative entry plus duplicate replicas.

Corrections and compensating entries are additive and apply only where the key
has a single authoritative value. They never delete or rewrite anything.

### State semantics

`unknown`, `unavailable`, `unsupported`, `stale`, `denied`, `unsafe`,
`conflict` and `zero` are distinct states.

* An absent observation is **not** zero: a category with no evidence has no
  authoritative keys at all, while an entry that asserts zero is one
  authoritative key with quantity zero.
* Freshness is derived at query time from the stored observation time, the
  caller's evaluation time and a maximum age. It is never stored, so reopening a
  ledger cannot make recovered evidence fresh.
* A tick-based clock domain without a declared tick rate cannot be classified:
  freshness stays unknown instead of defaulting to fresh or stale.
* An unknown observation time is the sentinel `INT64_MIN`, deliberately not
  zero.

## Authority, generations and fencing

Identity, generation and mutable state are separate:

| Concept | Type | Meaning |
| --- | --- | --- |
| ledger incarnation | 16-byte identity | which ledger this is; immutable for a store |
| store UUID | 16-byte identity | which directory tree this is |
| committed sequence | 64-bit | position of a committed entry, monotonic |
| source generation | 64-bit | generation of the measuring source |
| source revision | 64-bit | revision of one observation from one source instance |
| source epoch | 64-bit | re-provisioning epoch of a source instance |
| manifest generation | 64-bit | ordinal of a published durable manifest |
| writer epoch | 64-bit | ordinal of a writer session; strictly increases |
| attempt | 64-bit | retry counter carried by a caller |

* **Single writer.** Writer authority is an exclusive operating-system byte-range
  lock on `WRITER.LEASE`. No in-process mutex participates in cross-process
  safety; the exclusion is enforced by the operating system and is released by
  the operating system when a process dies.
* **Readers.** A read-only session takes a shared lock, so it cannot run while a
  writer session holds the lease. Read-only verification and audit read committed
  bytes only and can run while a writer is active.
* **Epoch handoff.** Every writer session raises the lease epoch by one. A store
  whose committed manifest was published by a newer epoch than the lease records
  is refused, so a store never reopens from behind a newer writer.
* **Fence on every append.** Before publishing, the writer re-reads the lease
  through its own locked handle and refuses to publish unless the lease still
  names exactly this session and epoch.
* **Explicit recovery.** A missing or destroyed lease is refused
  (`stale-writer`) and requires the caller to pass `recover_lease`, which
  re-establishes authority from the committed manifest: the epoch never
  decreases and the rollback floor is raised to the committed generation, so
  recovery cannot rewind the store. Recovery is reported by the store audit.
* **Rollback floor.** The lease records the highest committed generation. A store
  whose best valid generation is below that floor is refused as rolled back.
* **Stale authority.** Every state-dependent mutation states the authority it was
  planned against (`expected_head`, `expected_generation`,
  `expected_incarnation`) and is refused with `stale-authority` when that
  authority has moved — except for an idempotent replay of an already accepted
  attempt, which returns the prior result first.

## Idempotency

Two independent layers:

* **Event identity** is a pure function of the canonical entry content. Any
  replay of the same content, under any request id, returns the originally
  committed sequence and digest and appends nothing. This is permanent for the
  life of the incarnation.
* **Request id** is a caller supplied key retained in a bounded window
  (`StoreLimits::idempotency_window`, default 4096, configurable up to
  1,000,000). A replay inside the window returns the prior result; a request id
  reused for different content is refused (`request-id-conflict`). Replays
  outside the window are still caught by event identity.

Retries survive lost responses: the replay check runs **before** any generation
or head check, so a retry with a now-stale head still returns the prior accepted
result.

## Persistence and recovery

The store is a versioned, integrity-checked, append-only segment format with
alternating manifest generations. The full byte-level specification is in
[docs/FORMAT.md](docs/FORMAT.md); the essential guarantees are:

* an append is acknowledged only after the new manifest generation has been
  written to its slot, **flushed durably** and re-read and verified — that flush
  is the documented commit point;
* unfinished publication leaves the previous generation intact: the next open
  adopts exactly one whole verified generation or refuses, and never stitches
  partial state together;
* uncommitted trailing bytes and unreferenced segment files are reported as
  residue and discarded by the next writer session, never adopted;
* corruption, single-bit mutation, truncation, omission, duplication,
  reordering, segment substitution from another store, manifest rollback and a
  destroyed lease are all detected or refused;
* retention retires whole sealed segments while preserving the chain anchor of
  the last retired entry, so post-compaction verification still depends on all
  prior history.

## Building

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Options: `ENERGY_LEDGER_BUILD_CLI`, `ENERGY_LEDGER_BUILD_EXAMPLES`,
`ENERGY_LEDGER_BUILD_BENCHMARKS`, `ENERGY_LEDGER_BUILD_TESTS`,
`ENERGY_LEDGER_STRICT_WARNINGS` (default ON: `/W4 /WX /permissive-` on MSVC,
`-Wall -Wextra -Wpedantic -Wconversion -Werror` elsewhere) and
`ENERGY_LEDGER_ENABLE_ASAN`.

## Command line

```
energy-ledger init <store> [--force]
energy-ledger open <store> [--read-only] [--recover-lease] [--json]
energy-ledger verify <store> [--shallow] [--json]
energy-ledger audit <store> [--json]
energy-ledger format [--json]
energy-ledger append <store> --record <file.json> [--request-id ID]
                        [--expected-head N] [--expected-generation N]
energy-ledger import <store> --records <file.jsonl>
energy-ledger inspect <store> --sequence N | --event-id HEX [--json]
energy-ledger rollup <store> --object REF [--generation G] [--interval I] [--json]
energy-ledger reconcile <store> --object REF [--generation G] [--interval I] [--json]
energy-ledger provenance <store> --sequence N [--json]
energy-ledger history <store> [--object REF] [--event-id HEX] [--from N --to M] [--json]
energy-ledger correct <store> --target N --record <file.json>
                        [--mode correction|supersession|compensating]
```

Freshness evaluation options for queries: `--eval-time T --max-age MS
--clock-domain D --time-basis B --ticks-per-second N`.

Exit codes: `0` success, `1` usage, `2` library error, `3` store busy,
`4` verification failed.

### Record JSON

```json
{
  "kind": "delivered",
  "object": "facility/line-1",
  "generation": "gen-1",
  "interval": "2026-01-01T00:00Z/PT1H",
  "clock-domain": "utc",
  "time-basis": "utc-unix-seconds",
  "interval-start": 1767225600,
  "interval-end": 1767229200,
  "observed-at": 1767229200,
  "quantity": 1000,
  "unit": "kWh",
  "quality": "verified",
  "authority": "metering-service",
  "authority-tier": 3,
  "source-family": "revenue-meter",
  "source-instance": "meter-17",
  "source-revision": 4,
  "source-generation": 1,
  "source-epoch": 1
}
```

The reader is strict and bounded: integers only (authoritative quantities are
never floating point), no duplicate keys, bounded depth, bounded strings, valid
UTF-8 only.

## Examples

Six runnable examples build with the project and run under CTest. They are
ordinary library consumers and contain no accounting logic of their own:

| Example | Shows |
| --- | --- |
| `examples/01_lifecycle.cpp` | normal energy flow: delivered, consumed, wasted, commitment, curtailment, rollup, reconcile, verify, reopen |
| `examples/02_residual_unclassified.cpp` | an unexplained residual retained and reported, including a negative residual |
| `examples/03_correction_history.cpp` | a compensating record that preserves the original entry, provenance, and a refused weak supersession |
| `examples/04_idempotent_replay.cpp` | lost-response retry returning the prior result, replay by event identity, conflicting request id refused |
| `examples/05_stale_authority.cpp` | stale head refused, replay of an accepted attempt, second writer refused, epoch handoff, stale lease refused |
| `examples/06_reopen_recovery.cpp` | uncommitted residue left by an interrupted append is reported, discarded and never adopted |

## Library use

```cpp
#include "energy_ledger/energy_ledger.hpp"

using namespace energy_ledger;

auto ledger = Ledger::open("ledger-store");
AppendRequest request;
request.content = /* build an EntryContent */;
request.request_id = "req-0001";
request.has_expected_head = true;
request.expected_head = ledger.value().head_sequence();
auto result = ledger.value().append(request);
```

### Consuming the installed package

```cmake
find_package(EnergyLedger 1.0 REQUIRED)
target_link_libraries(my_app PRIVATE Summon::EnergyLedger)
```

`downstream/` is an independent out-of-tree consumer used to validate the
installed package:

```sh
cmake --install build --prefix /some/prefix
cmake -S downstream -B downstream-build -G Ninja \
      -DCMAKE_PREFIX_PATH=/some/prefix
cmake --build downstream-build
./downstream-build/energy-ledger-downstream
```

## Validation

The evidence record, including exact commands and results, is in
[docs/VALIDATION.md](docs/VALIDATION.md). Summary:

* four test suites whose cases follow the proof obligations rather than an
  arbitrary target count: core model, durable store, accounting/reference model,
  and real multiprocess authority and crash semantics;
* an exhaustive single-bit mutation sweep over every byte of every store file
  (descriptor, both manifest slots, lease, segments) requiring every mutation to
  be detected;
* exhaustive truncation of the active segment and of a manifest at every byte
  boundary;
* seeded randomized accounting streams compared after **every** accepted
  mutation against an independently written reference model;
* real child processes for writer exclusion, process-death lock release, crash
  injection at nine publication stages, and read-only verification during an
  active writer session;
* Release, Debug and AddressSanitizer builds, with first-party warnings as
  errors.

Coverage is bounded by the tests that exist; it is not a proof of correctness.
See the limitations section.

## Benchmarks

`energy-ledger-bench` measures **completed** operations only: an append is timed
through validation, canonical encoding, segment write, durable flush, read-back
verification, staging manifest write and flush, manifest slot publication,
durable flush of the slot, read-back verification, in-memory adoption and
idempotency bookkeeping.

All results are labelled **REAL** (measured on the host that ran them) with
**SYNTHETIC** generated input. No hardware validation is claimed.

Host: AMD Ryzen 7 9800X3D (8C/16T, 4.7 GHz), 64 GB RAM, Windows 11 Pro
26200, NTFS on a local volume. Toolchain: MSVC 19.44.35222, CMake 4.3.2, Ninja
1.13.2, Release build. Three independent runs of the whole benchmark; the table
reports the median run, and the parenthesised range is across the three runs.

| Operation | Scale | Result |
| --- | --- | --- |
| append (full durable commit) | 20,000 entries, 5,128,954 committed bytes | 160.2 ops/s, 6.24 ms per completed append (143.7 – 181.2 ops/s) |
| rollup (full resolution pass) | 20,000 entries, 200 queries | 17.75 queries/s, 56.3 ms per query, ~2.8 µs per entry resolved (17.7 – 18.6 queries/s) |
| close, reopen, replay and verify | 20,000 entries, 3 repetitions | median 285.3 ms (261.6 – 301.2 ms across runs) |
| read-only verification | 20,000 entries, 5,128,890 bytes | 98.2 ms (90.7 – 103.9 ms) |

The append figure is dominated by the two durable flushes and their read-back
verifications that the commit protocol requires; it is not a measure of enqueue
or submission latency. Reproduce with:

```sh
./build/bench/energy-ledger-bench --entries 20000 --rollup-queries 200 --reopen-repetitions 3
```

The benchmark removes its store before and after the run, verifies the committed
head after every append, verifies the store after every reopen, and refuses to
report results if any of those checks fail.

## Limitations

* **Platform.** Validated only on Windows x64 with MSVC 19.44. The POSIX file,
  locking and process paths are written and structurally complete but were not
  compiled or executed in this environment; treat them as unverified.
* **Threat model.** Integrity protects against corruption, torn writes,
  truncation, reordering, duplication, cross-store substitution and naive
  rollback. It does not protect against an adversary who can rewrite every byte
  of the store directory, including the descriptor, both manifest slots and the
  lease. There is no external root of trust, no secret key and no attestation.
* **Rollback floor.** The floor lives in the same store as the data it protects.
  It detects accidental or naive rollback of the directory, not a coordinated
  rewrite.
* **Single writer per store.** Writer sessions are exclusive; there is no
  multi-writer replication, no consensus and no networked store. A reader is
  excluded while a writer session is open.
* **Retention is irreversible.** Retired entries cannot be inspected afterwards;
  only the chain anchor that commits to them is preserved.
* **Instance scale.** The in-memory index holds one metadata record per retained
  entry, and the default ceiling is 262,144 retained entries with a 2 GiB
  committed byte ceiling. Both are configurable but they are real ceilings.
* **No hardware.** Nothing in this repository talks to a meter, a PDU, a UPS or
  any other device. Observations are accepted as evidence only.
* **Freshness is caller-supplied.** The ledger never reads a clock for
  accounting decisions: evaluation time and maximum age come from the caller.

## Licence

Apache License 2.0. See [LICENSE](LICENSE), [NOTICE](NOTICE) and
[CONTRIBUTING.md](CONTRIBUTING.md). Energy Ledger does not collect or transmit
telemetry.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.

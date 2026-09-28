# Validation record

Everything below was executed on the host described here. Nothing in this file
is projected, estimated or aspirational.

| Item | Value |
| --- | --- |
| Host | Gigabyte X870E AORUS MASTER |
| CPU | AMD Ryzen 7 9800X3D, 8 cores / 16 threads, 4.7 GHz |
| Memory | 64 GB |
| Operating system | Microsoft Windows 11 Pro, version 10.0.26200, build 26200 |
| File system | NTFS, local volume (`E:`) |
| Compiler | MSVC 19.44.35222 (Visual Studio 2022 Build Tools, toolset 14.44.35207) |
| Build system | CMake 4.3.2, Ninja 1.13.2 |
| Repository state | Energy Ledger 1.0.0, closure commit |

## 1. Build matrix

All three configurations build first-party code with `/W4 /permissive- /utf-8`
and **warnings as errors** (`/WX`). No warning is suppressed globally.

| Configuration | Command | Result |
| --- | --- | --- |
| Release | `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release` | clean build |
| Debug | `cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug` | clean build |
| AddressSanitizer | `cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENERGY_LEDGER_ENABLE_ASAN=ON` | clean build, compiles and links against `clang_rt.asan_dynamic-x86_64` |

The ASan executables need the MSVC toolset directory on `PATH` (the sanitizer
runtime DLL lives there); the suites were therefore run from a developer
environment. `ASAN_OPTIONS=detect_leaks=1` is **not** supported by MSVC
AddressSanitizer and was not used; leak detection is therefore not covered.

## 2. Test suites

`ctest --output-on-failure` in each configuration. No test carries a timeout,
watchdog, forced termination or process limit; every case runs to completion.

| Suite | Cases | Checks (Release) | What it proves |
| --- | --- | --- | --- |
| `el_tests_core` | 13 | 4,135 | units and checked arithmetic, strong identities, validation precedence, canonical encoding bijection, strict bounded JSON, digest known answers |
| `el_tests_store` | 17 | 23,492 | framing, commit points, recovery, corruption, truncation, reordering, duplication, store swap, rollback, retention |
| `el_tests_accounting` | 15 | 2,546 | resolution precedence, conflicts, residual preservation, freshness, selectors, overflow refusal, reference model equality |
| `el_tests_process` | 7 | 484 | real multi-process writer authority, process-death release, crash injection, live fence, read-only verification during a write session, CLI round trip (init, append, import, correct, supersession, inspect, rollup, reconcile, provenance, history, verify, audit, open, format) |
| examples | 6 | — | the six documented examples run to completion |
| `benchmark-smoke` | 1 | — | a small benchmark run completes, verifies its state and cleans up |

Release: **11/11 passed** (120 s). Debug: **11/11 passed** (126 s).
AddressSanitizer: **10/10 passed** (127 s, benchmark smoke excluded).
Total assertions: 30,657 across the four suites.

### 2.1 Exhaustive mutation and truncation

* `every_single_bit_mutation_is_detected` flips **every bit of every byte** of
  the descriptor, both manifest slots, the writer lease and every segment file
  (21,417 checks in one case) and requires each mutation to be refused by
  `open` or reported by `verify_store`. Zero undetected mutations.
* `truncation_at_every_byte_is_detected` truncates the active segment and a
  manifest to every length from zero to the file size (1,691 checks) and
  requires detection each time, then restores the bytes and requires the store
  to be healthy again.
* `canonical_decode_rejects_malformed_input` performs every single-bit mutation
  of a canonical content encoding (3,823 checks) and requires either refusal or
  a decoded content that re-encodes to exactly the mutated bytes and has a
  different content digest. Canonical encoding is therefore a bijection on the
  accepted set.

### 2.2 Reference model

`independent_reference_model_matches_seeded_random_streams` drives six seeded
random event streams (60 accepted mutations each, including deliberately
colliding keys, equal-rank conflicts, tier/epoch/revision variation and
duplicate replays) and compares the ledger rollup against an independently
written model — composed string keys, linear scans, plain integer sums, no
shared code — after **every** accepted mutation. 2,237 checks, all equal.
`reference_model_covers_corrections_and_supersessions` repeats the comparison
for a stream containing corrections and compensating entries.

## 3. Process-level and crash semantics

Every case below starts real child processes through `CreateProcessW`; nothing
is simulated in-process.

| Case | Result |
| --- | --- |
| Two processes contend for writer authority | the holder acquires; both probes are refused with `store-busy` (exit 3); after release a new process acquires |
| Writer process killed with `TerminateProcess` | the operating system releases the lock; a new process acquires; the store still verifies |
| Crash injection at nine publication stages | for each stage the child dies with the documented crash code; the store always adopts exactly one whole verified generation; stages before the commit point never commit, stages at or after the commit point never lose the entry, and the between-write-and-flush stage is accepted either way but must verify |
| Crash injection during segment rotation | no orphan segment survives; the store verifies after every stage and remains writable |
| Lease fence | a session whose lease was damaged refuses to publish (`stale-writer`); nothing is committed under dubious authority; explicit `recover_lease` re-establishes authority without lowering the epoch or rewinding the generation |
| Read-only verification during an active writer | `verify_store` and `audit_store` succeed while a writer session holds the lease |
| Epoch handoff | each session raises the epoch; a lease older than the committed manifest is refused |

## 4. Install, export and downstream consumption

```sh
cmake --install build --prefix <prefix>
cmake -S downstream -B downstream-build -G Ninja -DCMAKE_PREFIX_PATH=<prefix>
cmake --build downstream-build
./downstream-build/energy-ledger-downstream
```

`downstream/` is an independent CMake project that is not part of the repository
build. It resolves `find_package(EnergyLedger 1.0 REQUIRED)` against the
installation prefix, links `Summon::EnergyLedger`, and exercises create, append,
idempotent retry, rollup, reconcile, verify, close, reopen and audit through the
installed headers and library. Result: `downstream consumer ok`, exit code 0.

## 5. Benchmark methodology and results

`energy-ledger-bench` times completed operations only. An append measurement
covers validation, canonical encoding, the segment write, the durable flush of
the record, the read-back verification of the record, the staging manifest write
and flush, the manifest slot publication, the durable flush of the slot, the
read-back verification of the slot, in-memory adoption and idempotency
bookkeeping. Enqueue or submission latency is never measured.

Workload: 20,000 synthetic entries (SYNTHETIC input, REAL measurement) with 5,128,954
committed bytes, 200 rollup queries, 3 reopen repetitions, run three times on an
otherwise idle machine. The benchmark verifies the head after every append and
verifies the store after every reopen, and removes its store before and after
the run.

| Operation | Median run | Range across three runs |
| --- | --- | --- |
| append (full durable commit) | 160.2 ops/s, 6.24 ms/op | 143.7 – 181.2 ops/s |
| rollup (full resolution pass over 20,000 entries) | 17.75 queries/s, 56.3 ms/query | 17.7 – 18.6 queries/s |
| close, reopen, replay and verify | 285.3 ms median | 261.6 – 301.2 ms |
| read-only verification | 98.2 ms | 90.7 – 103.9 ms |

No before/after pair is published: no implementation was replaced, so there is
no isolated variable to compare.

## 6. Material defects found and fixed during hardening

Every item below was found by running the software, is reproducible, and was
fixed and re-tested.

1. **Manifest segment table misparsed.** The decoder never consumed the two
   reserved header words, so the segment table was read 16 bytes early and every
   rotated store was rejected as having non-monotonic segment identifiers.
2. **Manifest trailer digest covered the wrong range.** The encoder hashed the
   header, the table *and* the first 16 bytes of the trailer, so no manifest ever
   verified on reopen.
3. **Stored digests were hashed instead of compared.** The descriptor and lease
   digests were compared as `SHA-256(stored_digest)`, so a freshly written store
   was rejected with a digest mismatch.
4. **Rollup overwrote per-key totals.** `RollupResult` kept one category total
   per (generation, kind), silently discarding every other accounting key. Found
   by the reference model, fixed by aggregating keys with checked arithmetic.
5. **Retention left the entry count inconsistent.** `compact` removed sealed
   segments without reducing the retained entry count, so the post-compaction
   manifest failed its own consistency check and the store looked rolled back.
6. **Unvalidated reserved and trailer fields.** Segment header flags and reserved
   bytes, manifest trailer reserved bytes and seal trailers were not validated,
   so single-bit mutations in those bytes were undetected. Found by the
   exhaustive mutation sweep.
7. **Lease reads blocked by our own lock.** The writer read the lease through a
   second handle, which an exclusive byte-range lock blocks even inside one
   process, so every append failed with a stale-writer error.
8. **Residue and lease state misreported read-only.** A read-only session
   reported zero residue and an absent lease; both were corrected, and the audit
   now distinguishes "lease absent" from "lease held".
9. **Rollback was invisible to verification.** `verify_store` reported a rolled
   back store as healthy; it now reports the floor violation as a finding.
10. **Retired adjustment targets misclassified.** A correction against a retired
    entry was reported as `target-not-in-ledger`; it is now `entry-retired`.
11. **Directory creation was single level.** `Ledger::create` failed when an
    intermediate directory was missing.
12. **Descriptor encoder emitted the wrong length.** A runtime length assertion
    caught a 72-byte encoding of a 64-byte header before it could be written.
13. **Header flags unvalidated.** The common header flags word is now required to
    be zero, closing the last undetected mutation site.
14. **No recovery path for a destroyed lease.** A damaged lease made a store
    permanently unopenable. Explicit, audited `recover_lease` was added; it can
    only raise the epoch and the floor.

## 7. Genuine limitations of this validation

* **Platform.** Everything above is Windows x64 with MSVC. The POSIX branches of
  the file, lock and process layers are written but were neither compiled nor
  executed here; they are unverified.
* **Reparse points.** This environment cannot create junctions or symbolic links
  ("the file or directory is corrupted and unreadable" on an NTFS volume), so the
  reparse-point rejection path is implemented and the case reports that it was
  not exercised. Every other path-validation rule (traversal, device names,
  reserved characters, control characters, overlong input, directory where a file
  is required) is exercised.
* **Leak detection.** MSVC AddressSanitizer does not support
  `detect_leaks`; memory-leak checking is not covered. ASan was run for memory
  errors with the supported options.
* **No hardware.** No meter, PDU, UPS or other device was involved. All evidence
  is software-generated and every measurement above is labelled REAL only with
  respect to this software.
* **Coverage is bounded by the tests that exist.** A defect outside the exercised
  surface would not have been found by this validation.

## 8. Closure

The closure commit, annotated tag, remote reference verification and fresh-clone
reproduction are recorded in the release notes of the tag and in the repository
history. The tree contains no build output, store, benchmark residue or
temporary file; `git status --porcelain` is clean apart from intentionally
tracked sources.

# Contributing to Energy Ledger

Energy Ledger is a Data Center Control Plane (DCCP) Tranche 3 repository
maintained by Summon Software Labs. Contributions from individuals and
organizations are welcome under the terms of the Apache License 2.0.

## Licensing of contributions

By submitting a contribution you agree that it is licensed under the Apache
License 2.0, as described in section 5 of the [LICENSE](LICENSE). There is **no**
Contributor License Agreement to sign and no copyright assignment. You keep the
copyright to your contribution.

Do not add `Co-authored-by` trailers, generated-by notices, or attribution lines
that you cannot justify; commit authorship is recorded by Git itself.

## Scope and boundaries

Keep changes inside the repository's documented systems boundary — the durable
accounting ledger for facility electrical energy. Energy Ledger does not own
power capacity, electrical topology, control policy, feed authority, device
actuation, load-shedding selection, billing, market settlement, or telemetry
collection. Changes that pull those concerns into this repository will be
declined.

## Before you open a pull request

1. Build both configurations with warnings-as-errors:

   ```
   cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build/release
   cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
   cmake --build build/debug
   ```

2. Run the complete test suite in both configurations:

   ```
   ctest --test-dir build/release --output-on-failure
   ctest --test-dir build/debug --output-on-failure
   ```


3. Keep the public API strongly typed. New identities, incarnations, epochs,
   revisions and external references must be distinct types; do not introduce a
   generic integer where a semantic distinction exists.

4. New accepted state transitions must state the authority they were planned
   against, must be idempotent under retry, and must not rewrite committed
   history.

## Code quality expectations

- C++20, standard library only. A third-party dependency needs a written
  justification covering correctness, packaging and validation.
- First-party warnings are errors: MSVC builds with `/W4 /WX /permissive-`,
  other compilers with `-Wall -Wextra -Wpedantic -Wconversion -Werror`.
  Do not disable a warning globally; fix the defect or suppress it narrowly at
  the site with a comment explaining why.
- Authoritative accounting arithmetic is checked integer arithmetic. No floating
  point in authoritative quantities, no silent clamping, no wraparound.
- Persistence changes must come with a format version decision and adversarial
  store tests (corruption, truncation, reordering, duplication, rollback).
- Documentation describes only behaviour that is implemented and verified.

## Telemetry

Do not introduce telemetry transmission of any kind.

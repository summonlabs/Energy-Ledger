# Downstream consumer

An independent out-of-tree CMake project that consumes the installed Energy
Ledger package through `find_package`. It is intentionally not part of the
repository build: it must compile and run against an installation prefix with no
knowledge of the source tree.

```sh
cmake --install <build-directory> --prefix <prefix>
cmake -S downstream -B <downstream-build> -G Ninja -DCMAKE_PREFIX_PATH=<prefix>
cmake --build <downstream-build>
<downstream-build>/energy-ledger-downstream
```

The consumer creates a store, appends delivered and consumed energy, retries an
accepted request and checks that it replays, rolls up and reconciles the
interval, verifies the store, closes and reopens it, and checks the recovered
head. It exits non-zero if any expectation fails.

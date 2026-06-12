# DuckStation fork patches

The submodule gitlink stays pinned to clean upstream; our modifications live here as
patch files. After `git submodule update --init`, apply with:

```
cd vendor/duckstation && git apply ../../patches/duckstation/*.patch
```

- `0001-regtest-gpudump-cli-and-startup-fixes.patch`
  - Adds `-gpudump <path>`, `-gpudumpstart <frame>`, `-gpudumpframes <n>` to
    duckstation-regtest (core API existed, regtest only exposed replay).
  - Fixes regtest init order: `Core::ProcessStartup()` calls
    `Achievements::ProcessStartup()`, which reads the base settings layer — but regtest
    created that layer afterwards (null-deref SIGSEGV on startup). Reordered to match the
    Qt frontend (config first). Upstream bug.
  - Adds missing `<condition_variable>/<deque>/<functional>/<mutex>` includes in
    regtest_host.cpp (fails to build on Fedora 44 libstdc++ where the old transitive
    include chain is gone). Upstream bug.

Note: DuckStation is CC-BY-NC-ND-4.0 — these patches and the modified build are for
local use, not redistribution.

# DuckStation fork patches

The submodule gitlink stays pinned to clean upstream; our modifications live here as
patch files. After `git submodule update --init`, apply with:

```
cd vendor/duckstation && git apply ../../patches/duckstation/*.patch
```

- `0001-regtest-gpudump-cli-and-startup-fixes.patch`
  - Adds `-gpudump <path>`, `-gpudumpstart <frame>`, `-gpudumpframes <n>` to
    duckstation-regtest (core API existed, regtest only exposed replay).
  - Adds `-inputscript <path>`: replays pad-1 input from lines of
    `<start_frame> <end_frame> <Button>` (digital pad button names); forces Pad1 type
    to DigitalController.
  - Fixes regtest init order: `Core::ProcessStartup()` calls
    `Achievements::ProcessStartup()`, which reads the base settings layer — but regtest
    created that layer afterwards (null-deref SIGSEGV on startup). Reordered to match the
    Qt frontend (config first). Upstream bug.
  - Adds missing `<condition_variable>/<deque>/<functional>/<mutex>` includes in
    regtest_host.cpp (fails to build on Fedora 44 libstdc++ where the old transitive
    include chain is gone). Upstream bug.

- `0002-wide60-realtime-interpolation.patch` (applies on top of 0001)
  - Adds `src/core/gpu_wide60.{h,cpp}`: real-time primitive-level frame interpolation.
    Captures each logic frame's backend command list (tee at `GPUBackend::PushCommand`),
    segments by GP1(0x05) flips (word-count boundary: GP1 bypasses the GP0 FIFO, so the
    boundary fires once all words pushed before the GP1 are consumed), matches draws
    across frames (DMA source address order + type/texcoord fingerprints, difflib-style
    alignment, displacement gate in buffer-relative coords), and at the in-between vblank
    replays the new frame's list with lerped vertices - then the original a vblank later.
    Replays skip UpdateVRAM/CopyVRAM and re-apply live drawing-area/CLUT state.
  - Enable with regtest `-wide60` or env `PSXPORT_WIDE60=1` (any frontend, incl. Qt GUI).
  - Invisible to the emulated machine: no FIFO/timing perturbation.

Note: DuckStation is CC-BY-NC-ND-4.0 — these patches and the modified build are for
local use, not redistribution.

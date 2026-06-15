# gpu_differ — cross-renderer GP0 differ (ours vs Beetle, pixel-exact)

Directly compares **our** rasterizer (`runtime/recomp/gpu_native.c`) against the **Beetle/mednafen**
GPU (`vendor/beetle-psx/mednafen/psx/gpu.c`, our oracle) on **identical GP0 input**, so any
output-VRAM difference is a pure rasterizer-fidelity difference (blend, modulation, dither, edge
coverage) — **no live game-state alignment needed.**

This sidesteps the trap that defeated earlier screenshot comparisons: our HLE port and the
full-emulation oracle run at different frame timings / game states, so you cannot align them by
frame number. Feeding the *same captured primitive stream* to both rasterizers removes that
variable entirely.

## Pipeline

1. **Capture** (our live port). Env `PSXPORT_GPUTRACE="FRAME[:path]"` records, for one target gpu
   frame, a snapshot of VRAM at frame start + the exact GP0 word stream fed to `gpu_gp0()` that
   frame. Default path `scratch/bin/gp0trace.bin`. File format `GP0TRC01` (see gpu_native.c).

   ```
   PSXPORT_NOWINDOW=1 PSXPORT_NOAUDIO=1 PSXPORT_FORCE_BUTTONS=FFF7 \
   PSXPORT_NATIVE_FRAMES=4000 PSXPORT_GPUTRACE=3000:scratch/bin/gp0trace_f3000.bin ./run.sh
   ```

   **Multi-frame sweep (one run, many scenes):** pass a comma-separated frame list and a path
   PREFIX; each frame is written to `<prefix>_f<N>.bin`. The run is deterministic under
   `PSXPORT_FORCE_BUTTONS`, so frame N is reproducible. Use this to validate the rasterizer across
   menu + several gameplay scenes, not just one frame:
   ```
   PSXPORT_GPUTRACE="600,1000,1400,1800,2200,2600,3000,3150:scratch/bin/sweep/t" ... ./run.sh
   ```
   Then loop each trace through the oracle + ours + diff (`for t in <prefix>_f*.bin; do wide60rt …
   -gpureplay "$t" beetle.vram; replay_ours "$t" ours.vram; diff.py ours.vram beetle.vram …; done`).
   Note the GAME double-buffers:
   the frame being drawn alternates between the back buffer at VRAM (0,256) and the front at (0,0), so
   diff BOTH `--region 0,256,320,240` and `0,0,320,240` and take whichever holds this frame's draws.

2. **Replay through Beetle** (the oracle binary, software renderer, 1x — do NOT set
   `PSXPORT_INTERNAL_RES`). Seeds Beetle's VRAM with the captured initial VRAM, replays the words
   via `GPU_WriteDMA`, dumps Beetle's resulting VRAM:

   ```
   make -C runtime
   runtime/wide60rt "$TOMBA2_CHD" -bios bios \
       -gpureplay scratch/bin/gp0trace_f3000.bin scratch/bin/beetle_f3000.vram
   ```

3. **Replay through ours** (standalone, links gpu_native.c only):

   ```
   bash tools/gpu_differ/build.sh        # -> scratch/bin/replay_ours
   scratch/bin/replay_ours scratch/bin/gp0trace_f3000.bin scratch/bin/ours_f3000.vram
   # optional provenance: append VX VY (absolute VRAM coord) + set PSXPORT_PROVAT=1 to print which
   # prim our renderer used to write that pixel (op/tex/semi/blend/clut/tp/vertex color):
   PSXPORT_PROVAT=1 scratch/bin/replay_ours trace.bin out.vram 144 447
   # optional per-pixel math trace (both renderers): PSXPORT_PIXTRACE="VX,VY" dumps every prim that
   # writes that pixel with sampled texel + interpolated color + modulated output. gpu_native.c
   # carries [pixtrace ours]; Beetle's gpu_polygon.c carries the matching [pixtrace beetle].
   ```

4. **Diff** (python, no deps beyond optional PIL for `--ppm`):

   ```
   tools/gpu_differ/diff.py ours.vram beetle.vram [--region X,Y,W,H] [--tol N] [--top N] [--ppm DIR]
   ```
   - `--region` restricts to a rectangle (e.g. the GAME back buffer `0,256,320,240`).
   - `--tol N` ignores pixels whose every per-channel |Δ| ≤ N — filters dither/rounding noise so
     real bugs surface (dither perturbs ≤ ~1-3 in 5-bit; use `--tol 3`).
   - `--ppm DIR` writes ours/beetle/absdiff PPMs of the region (absdiff amplified ×32).
   - Prints differing-pixel count, bbox, max-Δ histogram, and the top-N pixels with both colors.

## Notes / gotchas
- The GAME frame double-buffers: draws go to the back buffer at draw offset `(0,256)`, so the
  drawn content lands at VRAM `y+256`. Query/diff that region; display coord = VRAM coord − 256.
- Both renderers must run at **1x** (the Beetle `-gpureplay` path memcpys a flat 1024×512 VRAM).
- The Beetle side reuses the full oracle boot (needs the CHD + BIOS) then replays — `GPU_ReplayBegin`
  (gpu.c) flushes the FIFO and grants draw-time budget so replayed prims rasterize immediately.

## First win (journal later 44)
Found & fixed the "character/item shadow renders as a hard black wedge" bug: our gouraud-textured
modulation used vertex A's color held **flat** across the whole prim instead of the per-pixel
**interpolated** color, collapsing a soft shadow gradient into a uniform dark block. The differ
pinned it (PROVAT → op 3C gouraud-textured opaque, then PIXTRACE → `vcol=(32,32,32)` flat). After
the fix, back-buffer real-divergence (|Δ|>3) dropped 41%→17%.

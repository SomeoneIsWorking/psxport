# Graphics debugging — workflow + tools (Tomba!2 port)

_Committed canonical copy of the `gfx-debug` skill (`.claude/` is gitignored). Keep both in sync._
_Read this FIRST before debugging any rendering issue._

## The methodology (2026-06-20): the engine OWNS its render — verify on the LIVE game

There is **no oracle and no render-diff tool anymore** (the Beetle reference emulator + `gpu_differ` +
all the compare scripts were removed by user directive). The engine OWNS its world, projection, and
render ordering PC-native — so there is nothing PSX to diff against. A "renders wrong" bug is found by
inspecting **the running port**, not by replaying a GP0 stream through a second renderer.

For ANY "something renders wrong" problem (water, fade, corruption, color, geometry, order):

1. **Inspect the LIVE game's render state** — drive the port to the scene via the REPL, then use the
   diagnostic channels (enabled at runtime with the `debug <chan>` REPL/debug-server command — NOT an
   env var) and the on-demand inspection commands:
   - `debug scene` — classified per-frame display list (poly/rect/fill/VRAM-copy/env counts, fade/copy
     details). Live debug server: `scene` (on-demand `gpu_scene_dump`).
   - `debug gte` — GTE ops + lighting/fog/projection control regs.
   - live debug server `provat x y` — which prim (op/clut/texpage/node) last wrote a displayed pixel.
   - live debug server `vkvram x y w h <path>` — read back an arbitrary VK VRAM region to a PPM (verify
     a texture/CLUT page is actually where the sampler expects it).
   - `debug fade` / `debug semi` — fade brightness / semi-transparent prim blend+bbox details.
2. **Build it and have the USER eyeball.** The agent CANNOT self-verify a render — it builds, captures a
   headless screenshot (`PSXPORT_VK_HEADLESS=1` + the REPL `shot <path>` command, VK-readback PPM), and
   asks the user (who is at a Mac and can eyeball pics / pull a push). NEVER conclude a render is correct
   from a single still you looked at — stills hide bugs (water "looked fine" while the user saw it
   broken). The user is the visual authority.

**The engine owns ordering — never re-sync with the PSX.** If something draws in the wrong order (e.g. a
background painted over the scene), the fix is to give the engine the correct ordering RULE (its own
depth buffer for 3D, its own 2D layer/sort for backgrounds/HUD) — NOT to read the PSX OT / GP0 order /
z-otz. There is no PSX draw order to consult anymore.

## Capturing a screenshot to send the user (headless, no display)
```
printf 'newgame\nskip 200\nrun 60\nshot scratch/screenshots/x.ppm\nquit\n' \
  | PSXPORT_VK_HEADLESS=1 PSXPORT_REPL=1 PSXPORT_NOAUDIO=1 scratch/bin/tomba2_port scratch/bin/tomba2/MAIN.EXE
# convert + send: magick/convert x.ppm x.png, then SendUserFile
```
`newgame` pulses to the GAME prologue; `skip N` advances the intro into the field; `run N` steps N
frames; `shot` reads back the VK image over the display region (works headless). Drive elsewhere with
`press`/`release`/`tap <btn>` + `run`.

## The render toolbox (what's left)

| Tool | What it does | Invoke |
|------|--------------|--------|
| **REPL `shot <path>`** | VK-readback of the presented region to a PPM (works headless). The way to capture a frame for the user. | piped REPL command |
| **`debug <chans>`** | Enable diagnostic channels at runtime (scene/gte/fade/semi/provat/objz/ndepth/…) — replaces the old per-flag env vars. | REPL or debug-server command |
| **`debug fadeshot`** | On every recomp screen-fade call `FUN_8007E9C8(color=a0)` (interp.cpp hook), log `call=N color=0xRRGGBB ra=` and capture `s_tex` to `scratch/screenshots/fade_NNN.ppm`. Deterministically captures a transition's fade frames (frame-indexed shots DRIFT across launches). Headless shows the CORRECT single menu→cutscene fade-in; the corrupt EXTRA menu fade only forms windowed (later-250). | `debug` channel |
| **live debug server** | NON-BLOCKING TCP server (`runtime/recomp/dbg_server.c`) — inspect the port WHILE the user plays live (windowed). `r`/`rw`/`stage`/`scene`/`provat x y`/`vkvram`/`shot`/`debug <chan>`/`frame`. | `PSXPORT_DEBUG_SERVER=1` (port 5959); `tools/dbgclient.py <cmd...>` |
| `PSXPORT_VK_HEADLESS=1` | Offscreen VK (no window/display) — deterministic; the basis of the headless screenshot. | env (run mode, not a behavior gate) |
| **`debug otattr` + REPL `otattr`** | OT/GTE SUBMISSION ATTRIBUTION: traces every guest-submitted packet-pool store and every GTE RTPS/RTPT back to the emitting guest fn (rec_dispatch shadow stack) + world-object node (same `cur_render_node` fallback the native submit path uses). Turn the channel on BEFORE the frame you want to inspect, then run the REPL `otattr` command to re-walk the last OT read-only and print `pool= op= fn= caller= node= beh@node+1C=` per packet, plus a per-(fn,node) GTE call-count histogram. Built for bug #45 (multi-quad batching) + bug #34 (dialog-panel emitter) — see `game/render/ot_attr.h`. Blind to fully-native (non-dispatched) draw paths — see the class comment for the limitation. | `debug otattr` then REPL `otattr` |
| **REPL `otattr watch/who/trace`** | LAST-WRITER PROVENANCE — answers "who WROTE this word" (any address, incl. scratchpad), independent of call-flow, for the staging-buffer case where `otattr`'s call-path attribution only names the batcher. `watch <addr> <len>` registers a region (8 slots, 64 KB total); `who <addr> [len]` dumps the word-granular last-writer, coalescing same-writer runs; `trace <addr>` prints the last writer + a copy-loop heuristic (does the writer's stores fan out across many 4KB pages this frame?) and points at a Ghidra follow-up when the source isn't statically determinable. Same shadow-stack signal as `otattr`, so it's ALSO blind past a plain (non-jalr/rec_dispatch) nested call — verified on bug #45's `0x8003F9A8`: last-writer matches call-path attribution exactly (no hidden RAM-copy hop), so the true emitter leaf there needs a Ghidra decompile of `0x8003F9A8`, not this tool. See `docs/config.md` `otattr` section. | `debug otattr` then REPL `otattr watch/who/trace` |
| **`PSXPORT_PRIMAT="x,y"`** | At a DISPLAY pixel, log EVERY prim covering it (point-in-triangle) with is3d/bg/billboard/per-vertex dep/op/tp/clut/col/bbox — from BOTH the gp0 OT-walk (`gpu_native.cpp` gp0_exec) AND the queue/world path (`gpu_emit_rq_item`, tagged `[primat-rq]`). USE THIS, not `provat`, for VK polys: provat tracks CPU `s_vram` and is BLIND to the VK-teed polygons. Tells you what actually contests a pixel and at what depth. | env or `debug`-channel cfg |
| **`PSXPORT_PAINTFG=1`** | Force every gp0 2D-FG (HUD-band) poly to solid magenta — instantly shows how much of the frame is still flat 2D-band (un-owned) vs real-depth world geometry. (later-229: revealed the seaside field is ~99% 2D-band.) | env / cfg |
| **`PSXPORT_PAINTER=1`** | Force PURE PSX OT painter order for EVERY prim (gp0 tee: is3d=0, no bg split) — composites exactly as the PSX ordering table would. Render the SAME scene with `=1` and `=0` and pixel-diff: differing pixels = where native per-pixel depth changes the picture (the object-occlusion bug — terrain/atlas not obeying world-depth). If `=1` makes the bug disappear, native-depth ordering is the cause. Headless start-field diff was only 39px (that view is ~99% 2D-band already, so native≈painter); run it WINDOWED where the bug shows. | env / cfg (`gpu_native.cpp` gp0_exec) |
| `tools/render_cmp.py` | Native-vs-PSX render compare at 1x/4:3/30fps → `scratch/screenshots/cmp_triptych.png` + diff %. A DIAGNOSTIC GATE (real-depth ownership is the target, NOT matching PSX). `g_render_psx`/`PSXPORT_RENDER_PSX` renders the field via the PSX recomp path for the compare. | CLI |
| `tools/disas.py <addr> [--ram dump]` | MAIN.EXE / overlay disassembly with resolved load/store addrs — read the recomp reference body before reimplementing an engine fn. | CLI |

## Visual settings (widescreen / hi-res / SSAO / lighting / 60fps)
These are NOT env vars — they default OFF and are toggled live in the **F1 imgui overlay**, persisted to
`psxport_settings.ini` (gitignored), restored next launch (`runtime/recomp/mods.c`, `mods.h`). To
reproduce a user's look, set the relevant keys in `psxport_settings.ini` before launching. NB SSAO/light
currently have a known bug hiding some transparent surfaces (puddles/water) — see the journal.

## Honest limits
- **You cannot self-verify a render.** The user is the visual authority — build, send a pic, ask.
- Headless `shot` captures the display region (`s_disp_*`); for a widescreen run the wider FB content is
  rendered but the shot crops to the display width — note this when capturing widescreen.

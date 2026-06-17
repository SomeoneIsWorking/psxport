# Graphics debugging — toolbox + MANDATORY workflow (Tomba!2 port)

_Committed canonical copy of the `gfx-debug` skill (`.claude/` is gitignored). Keep both in sync._
_Exists because a session LOST TRACK of `gpu_differ` and reinvented a worse screenshot tool. Read this
FIRST before debugging any rendering issue, and before building any new comparison tooling._

## THE WORKFLOW IS NOT OPTIONAL — render-level diff FIRST, eyeball LAST

For ANY "something renders wrong" problem (water, fade, corruption, color, geometry), the order is:

1. **Rendering-level diff FIRST — `gpu_differ` (ours vs Beetle on the IDENTICAL GP0 stream).** This is
   the primary tool. It captures the exact GP0 word stream + start VRAM for a frame and replays it
   through BOTH our rasterizer and Beetle's GPU, then diffs the output VRAM. Any difference is a pure
   rasterizer-fidelity bug (blend/modulation/dither/edge/copy) with **no game-state alignment needed**.
   This is the truest "synced comparison" — same input to both renderers. USE THIS, do not screenshot.
2. **Game-state / GP0-stream inspection** if the diff is clean (→ the bug is in WHAT we draw, not how):
   `PSXPORT_SCENEDUMP` (classified per-frame display list), `PSXPORT_PROVAT` (which prim drew a pixel),
   `PSXPORT_GTEPROBE` (GTE op/lighting/projection), `PSXPORT_POLYDUMP`/`POLYAT`.
3. **Eyeball a screenshot ONLY when stuck** after 1–2 — i.e. when the render-diff is clean and the
   state-inspection didn't localize it, and you genuinely need a human (the user) to judge. Eyeballing
   and whole-frame pixel-diffing are LAST RESORTS, never the first move. They hid the water bug for a
   whole session (stills "looked fine"; the user saw it broken). Never conclude from a still alone.

**Corollary:** do NOT compare the port and oracle by running them to a frame number / latch and
screenshotting — their boot timing and attract demos DRIFT, so the states differ. `gpu_differ` removes
that variable. (`tools/dualcore.py` state-sync is fragile — see Limits.)

## IMPROVE THE TOOLS WHEN THEY FALL SHORT — do not hand-grind or reinvent

If a tool can't answer the question, **extend the tool**, don't grind it by hand and don't build a
parallel worse one. Before writing new tooling, grep this file + `tools/` for what exists. When you add
or fix a capability, update this doc + the skill in the same change. (Global rule "build tools, don't
re-reason"; this is its project-specific enforcement.)

## The toolbox (so you stop losing track)

| Tool | What it does | Invoke |
|------|--------------|--------|
| **`tools/gpu_differ/`** | **PRIMARY render diff.** GP0-stream replay: ours vs Beetle, pixel-exact VRAM diff. `README.md` there. | capture `PSXPORT_GPUTRACE="F[:path]"`; `bash tools/gpu_differ/build.sh` → `scratch/bin/replay_ours trace out.vram`; oracle `wide60rt <chd> -bios scratch/bios -gpureplay trace beetle.vram`; `tools/gpu_differ/diff.py ours.vram beetle.vram --region x,y,w,h` (try both buffers 0,0 and 0,256) |
| `tools/drive.py` | Interactive single-core driver (port OR oracle) over a FIFO. run/r/rw/watch/shot/dumpram/stage/tap. | `drive.py start native\|oracle`; `drive.py send "run 60" "stage"` |
| `tools/dualcore.py` | Runs port+oracle, syncs on a guest-RAM latch (default 0x800BE258==2 scene-active). **LIMITED** (see below). | `dualcore.py start; sync; step N; shot NAME` |
| `PSXPORT_SCENEDUMP=F` | Native classified display list for frame F (poly/rect/fill/VRAM-copy/env counts + fade/copy details). gpu_native.c. | env on any run |
| `PSXPORT_PROVAT="x,y"` | Per-pixel primitive provenance: which prim (op/clut/texpage/node) last wrote a displayed pixel. gpu_native.c. | env |
| `PSXPORT_GTEPROBE=F` | GTE ops executed + lighting/fog/projection control-reg snapshot. gte_beetle.c. | env |
| `PSXPORT_POLYDUMP=F` (+`POLYAT=x,y`) | Log every poly/sprite at frame F (optionally only those covering a point). gpu_native.c. | env |
| `PSXPORT_FADEDBG="a:b"` | Per-frame over [a,b]: max prim brightness, SEMI prim colour range, full-screen-semi count (`bigsemi`), disp + draw origin. Renderer-independent (same tee under SW/VK) → settles "is the bright frame in the GP0 or invented by VK?". gpu_native.c. | env |
| `PSXPORT_SEMIDUMP=F` | Log each SEMI prim at frame F: blend mode (0=avg,1=add,2=sub,3=add/4) + colour + bbox + draw-Y. Found the stacked subtractive fade tiles behind the intro fade flash. gpu_native.c. | env |
| state capture | **port:** `dumpram`, transplant `PSXPORT_TRANSPLANT="F:ram:vram"`, `gpu_vram_load`. **oracle:** `dumpram`, `dumpvram` (REPL), `-loadstate`/`-savestate`, `PSXPORT_VRAMDUMP`, `PSXPORT_FRAMEDUMP`. | REPL / env |
| screenshots | port `shot PATH` (SW) or `PSXPORT_VK_SHOT=F` → `scratch/screenshots/vk_live.ppm` (VK); oracle `shot PATH`. | LAST-RESORT visual only |
| **live debug server** | **NON-BLOCKING** TCP server (`runtime/recomp/dbg_server.c`) — inspect the port WHILE the user plays live (windowed), unlike the blocking `PSXPORT_REPL` FIFO. Commands serviced on the main thread once/frame. `r`/`rw`/`stage`/`scene` (on-demand `gpu_scene_dump`)/`provat x y` (on-demand `gpu_provat_display`)/`shot [path]`/`gputrace [path]` (arm a gpu_differ capture of the next frame)/`sbs [0\|1]` (VK-vs-SW side-by-side present)/`frame`. | run with `PSXPORT_DEBUG_SERVER=1` (port 5959); drive with `tools/dbgclient.py <cmd...>` or interactively (`tools/dbgclient.py`) |

## Honest tool limits (fix these when they block you — don't work around by eyeballing)
- **`gpu_differ`/oracle replay needs Beetle SW @ 1x** (flat VRAM). Don't set `PSXPORT_INTERNAL_RES`.
- **`dualcore.py` sync is weak:** it advances each core to a latch SEQUENTIALLY; the attract demos are not
  frame-locked so they DRIFT, and the latch can catch a black load frame. State-transfer (transplant)
  was tried but RAM-only transfer is fragile (CPU regs/PC not transferred → port resumes its own path,
  showed stale SCEA). If you need true game-state sync, the route is a FULL savestate transfer or the
  gpu_differ stream replay (preferred). **Improve dualcore or prefer gpu_differ — do not fall back to
  eyeballing drifted screenshots.**
- VK widescreen FB has a known 1x rasterization-gap (vertical bars); clean at SS=2. Separate from game.

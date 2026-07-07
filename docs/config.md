# Configuration & debug flags — `cfg` module + the REPL-driven `debug` channel set

Read this before adding a new `getenv("PSXPORT_…")`. The repo accumulated ~105 ad-hoc env flags, each
with its own `static int x=-1; if(x<0) x=getenv(...)` boilerplate. That is now centralized.

## The 5 canonical run flags (path selection) — see CLAUDE.md for full vocab

| flag | selects | notes |
|---|---|---|
| (none) | pc_faithful + pc_render | Default. Byte-exact target for SBS. Currently broken (Job#1). |
| `PSXPORT_GATE=1` | recomp_path + pc_render | Substrate runs gameplay; native renderer. Works, render issues. |
| `PSXPORT_RENDER_PSX=1` | pc_faithful + psx_render | Substrate renderer only. Faithful still broken. |
| `PSXPORT_GATE=1 PSXPORT_RENDER_PSX=1` | recomp_path + psx_render | THE REFERENCE. Works perfectly. |
| `PSXPORT_SBS_MODE=full` | dual-core byte-compare | Core A = pc_faithful, Core B = recomp_path. Job#1 harness. |

The per-fork shortcut bool is `Game::mPcSkip` — see the class comment on `runtime/recomp/game.h`. Default
`mPcSkip=true` (shortcuts on); SBS forces it `false` so the faithful branch of every fork is exercised.

**No new env GATING (2026-06-20).** The PC-native port is the only path — do NOT add a `PSXPORT_*` flag
that branches game behavior/render/features. Visual settings (widescreen/hi-res/SSAO/light/60fps) are
the F1 overlay + `psxport_settings.ini` (`runtime/recomp/mods.c`), not env. Diagnostics are the REPL
`debug <chan>` command (below), not env. `cfg_*` remains only for genuine launch config (disc path,
window/headless run mode).

## The rule: don't call `getenv` — use `cfg` (`runtime/recomp/cfg.h`)
```c
#include "cfg.h"
cfg_on("PSXPORT_FOO")     // boolean config/feature flag: env present and != "0" -> 1   (cached)
cfg_int("PSXPORT_FOO", d) // integer-valued flag (frame number, scale, port…), default d (cached)
cfg_str("PSXPORT_FOO")    // string-valued flag (paths, "x,y" coords); NULL if unset    (cached)
cfg_dbg("chan")           // is debug CHANNEL `chan` on? set at runtime via the REPL `debug chan,chan…`
```
- Every lookup reads the environment **once** and caches. In hot paths (per-prim / per-GTE-op /
  per-store) still keep a local `static int x=-1; if(x<0) x=cfg_*(…)` so there is no per-call scan.
- `-w` is on in the build, so an int/pointer mix-up (e.g. mapping a *valued* flag to `cfg_dbg`) is **not**
  warned — but `-Wint-conversion` is still an **error**, so the compiler catches that specific mistake.
  Still: classify before you wire (boolean → `cfg_on`/`cfg_dbg`; anything whose value is read → `cfg_str`/`cfg_int`).
- `cfg_dump()` logs every active `PSXPORT_*` var once (boot-time visibility).

## Debug output is REPL-driven: `debug chanA,chanB` (or `debug all`, `debug` to clear)
~31 boolean `*_DBG` / `*LOG` / `*WATCH` / `VERBOSE` flags were collapsed into named channels, enabled at
runtime via the REPL / debug-server `debug` command (`cfg_dbg_set`), NOT an env var. Enable several
together: `debug spu,cdcmd,bgm`. Old → new channel:

| old env flag | channel | | old env flag | channel |
|---|---|---|---|---|
| `PSXPORT_AUDIO_LOG`    | `audio`   | | `PSXPORT_REDDBG`      | `red`       |
| `PSXPORT_AUDIO_RATE`   | `audiorate`| | `PSXPORT_RTPCALLER`  | `rtpcaller` |
| `PSXPORT_CARD_VERBOSE` | `card`    | | `PSXPORT_SCHEDDBG`   | `sched`     |
| `PSXPORT_CDCMD_DBG`    | `cdcmd`   | | `PSXPORT_SEQDBG`     | `seq`       |
| `PSXPORT_CDC_VERBOSE`  | `cdc`     | | `PSXPORT_SPINDBG`    | `spin`      |
| `PSXPORT_CD_VERBOSE`   | `cd`      | | `PSXPORT_SPU_DBG`    | `spu`       |
| `PSXPORT_ENGINE_DBG`   | `engine`  | | `PSXPORT_SPU_PROF`   | `spuprof`   |
| `PSXPORT_ENVDBG`       | `env`     | | `PSXPORT_STAGETL`    | `stage`     |
| `PSXPORT_FMV_DEBUG`    | `fmv`     | | `PSXPORT_STREAMDBG`  | `stream`    |
| `PSXPORT_GP1LOG`       | `gp1`     | | `PSXPORT_TEXTDBG`    | `text`      |
| `PSXPORT_GPU_LOG`      | `gpu`     | | `PSXPORT_UNPACKLOG`  | `unpack`    |
| `PSXPORT_LDHAZARD`     | `ldhazard`| | `PSXPORT_UPLOADLOG`  | `upload`    |
| `PSXPORT_OBJLOG`       | `obj`     | | `PSXPORT_VSYNCLOG`   | `vsync`     |
| `PSXPORT_OTDBG`        | `ot`      | | `PSXPORT_VRAMSCAN`   | `vramscan`  |
| `PSXPORT_CDCMD_DBG`    | `cdcmd`   | | `PSXPORT_FPS60_SDBG` | `fps60`     |
| (so e.g. `PSXPORT_CDCMD_DBG=1` → `PSXPORT_DEBUG=cdcmd`) | | | `PSXPORT_WS_SXHIST` | `sxhist` |

New channels (no legacy var): `schedf` (per-frame cooperative task0/1/2 state + GAME `sm[0x48/4a/4c/5c]`
trace, native_boot.cpp — for stage/scheduler debugging) · `stage` (GAME stage-machine native-ownership log,
game/core/engine.cpp) · `rqhist` (per-frame render-queue layer×opaque/semi histogram, render_queue.cpp — "is
the world even being queued?") · `ovload` (per-core MODE/AREA-slot overlay residency: logs each
`overlay_note_load` — `core A/B slot N <- TAG` — so you can see WHICH overlay each core thinks is resident
and WHEN it loaded; the tool that pinned later-273's "A00 code overlay never loaded on the PSX core",
overlay_router.cpp) · `recdep` (RECOMP-DEPENDENCY meter: histograms every substrate function `rec_dispatch`
routes to and dumps the top-40 by call count at exit, overlay_router.cpp — the metric for the "minimize
recomp" goal; run `PSXPORT_DEBUG=recdep` on a free-roam session to rank which substrate functions to own
natively next. 410 unique in free-roam; #1 is `rand` 0x8009A450 @ 86/frame). See journal later-168 /
engine_re.md "GAME stage state machine" · `silbbox` (dark-outline render-bug diag, `render_internal.h`
`sil_bbox_log`/`_i`: logs the screen bbox of every quad — native_gt3gt4's GT3/GT4/byte-packed submitters,
`ov_bg_tilemap_native`'s sky backdrop — that overlaps the coastal-ridge repro window x=5..30 y=134-138, see
`docs/findings/render.md` "Screen-fade transitions" / `scratch/handoff.md`).

Full-PSX (psx_fallback / SBS core-B) coroutine diagnostics (native_boot.cpp `ov_switch`): `sched` (coro
start/resume/out + task slot state) · `yieldpc` (per-yield `ra`/`r16`/`r29` + the stale-on-inner-frames
`waitloop` heuristic — prefer btyield) · `btyield` (at each coro yield: guest-stack scan AND a PRECISE
C-level `backtrace()` of the fiber thread = the recompiled `func_XXXX`/`ov_*_gen_*` call chain, so you can
see exactly which recomp function yields and read its callee-saved regs — the reliable tool for the
deep-field-coro freeze; see findings/sbs.md).

`dispatch` (engine_overrides.cpp) — logs every ENGINE-OVERRIDE hit at the global dispatch point
(`rec_dispatch` top): `[dispatch] fN core=A 0xADDR Class::method ra a0-a3`. Every native engine
wired by guest address (see CLAUDE.md "One global dispatch point") funnels through here, so this
channel is the uniform call trace across substrate→native and native→native dispatch calls.

`fadetrace` (screen_fade.cpp) — logs every native-path `ScreenFade::set` / `applyLeafCall` with the
mode+rgb, and edge-logs HOLD latched / released transitions. Pairs with
`PSXPORT_DISPWATCH=0x8007E9C8` (which surfaces every substrate fade dispatch with its guest stack).
Together they show BOTH sides of the fade caller graph — essential when a cutscene fadeout stays
stuck black: silent `fadetrace` + active `dispwatch` = the failing fade caller is a substrate SM
handler that needs porting native. Issue #27.

**`PSXPORT_DEBUG=chanA,chanB` env now works at launch** (seeded once in `cfg_dbg`, runtime/recomp/cfg.c) —
previously channels were ONLY settable via the REPL/debug-server `debug` command, so headless/SBS runs
couldn't enable one despite this doc claiming the env drove it. A later REPL `debug …` overrides the seed.

Render layer-isolation diags (value flags, `cfg_str`, gpu_native.cpp gpu_emit_rq_item) for "where did the
native world go?": `PSXPORT_ONLYWORLD=1` (emit ONLY RQ_WORLD), `PSXPORT_NOBG=1` (drop RQ_BACKGROUND),
`PSXPORT_NOHUD=1` (drop RQ_HUD). `PSXPORT_PRIMAT="x,y[,f0]"` gained an optional min-frame `f0` so the
6000-line cap isn't exhausted before the target scene (e.g. reach free-roam at f216 with `,400`).

## Flags that kept their own var (they carry a VALUE, not just on/off)
These stay as `PSXPORT_*` (read via `cfg_int`/`cfg_str`) because they take a frame number, coords, path,
or level — they can't be a bare channel:
- **Renderer / mode:** `VK` (default on), `SW_GPU`, `VK_NODEPTH`, `VK_TRITEST`, `VK_HEADLESS`,
  `GPU_WINDOW`, `WINDOWED`, `IRES`, `WIDE`, `FPS60`, `FPS60_GATE`, `FPS60_SYNTH`, `NATIVE_DEPTH`,
  `SSAO` (+ `SSAO_STRENGTH`/`SSAO_RADIUS`/`SSAO_BIAS`/`SSAO_RANGE`/`SSAO_VIZ`), `LIGHT`
  (+ `LIGHT_DIR`="x,y,z"/`LIGHT_AMBIENT`/`LIGHT_DIFFUSE`; SSAO+LIGHT share one deferred pass, `SSAO_VIZ`
  =2 shows normals, =3 shows the lit factor), `UI` (Dear ImGui mod-toggle overlay, windowed only —
  toggle live: wide/ires/fps60/ssao/light; ` or F1 to hide; forces native-depth + deferred infra on so
  the toggles work live; seeds g_mods in mods.c), `ATTACH`, `PROJPROBE`,
  `CULL`/`CULL_FAR`/`CULL_FOV`, `*_RECOMP` (`OT_/LZ_/GEOM_/RECOMP_OBJWALK`), `TRANSPLANT`.
- **SDL_GPU renderer (gpu_gpu.cpp):** `GPU_TRACE` (per-present src-VRAM occupancy + sampled disp region +
  readback nonzero count), `GPU_DEBUG` (enable the SDL_GPU device validation layer — slows pipeline
  compile, can trip the boot watchdog; raise `WATCHDOG_BOOT` when using it), `GPU_SELFTEST` (headless
  renderer regression test: render a known VRAM pattern through the present pipeline into an offscreen
  RGBA8 target and assert orientation + 1555 unpack, then exit 0=PASS/1=FAIL — runs `tools/test_gpu_render.sh`;
  no disc needed). `VK_HEADLESS` (offscreen, no window) and `FULLSCREEN`/`WINDOWED` are honored unchanged.
- **Boot / automation:** `NO_FMV`, `NOAUDIO`, `NOPACE`, `NOSKIP`, `NATIVE_FRAMES`, `AUTO_GAMEPLAY`,
  `AUTO_NEWGAME`, `SCEA_SKIP`, `WATCHDOG`, `REPL`, `DEBUG_SERVER`, `T2_NOSEQTICK`, `FMV_*`, `FORCE_*`.
- **Paths:** `TOMBA2_DISC`, `TOMBA2_CARD`, `DISC`.
- **Valued diagnostics (still named, gfx-debug.md documents them):** `SCENEDUMP`, `PROVAT`, `GTEPROBE`,
  `POLYDUMP`/`POLYAT`, `FADEDBG`, `SEMIDUMP`, `VK_SHOT`/`VK_SHOTSEQ`, `VK_DIFF`, `GPUTRACE`,
  `VRAMDUMP`/`VRAMDUMP_AT`, `RAMDUMP`/`RAMDUMP_FRAME`, `CLOBBERDUMP`, `CLUTWATCH`, `WWATCH`, `CW`/`CW_BT`,
  `XA_DBG` (level), `BGMDBG` (level), `GPU_DUMP`, `WAV`, `SS`, `SBS`.
- **Dual-core render-diff harness (`PSXPORT_DUALCORE=1`, class DualCore, dualcore.cpp):** `DC_N`
  (frames after gameplay-start, default 180, `cfg_int`), `DC_LO`/`DC_HI` (focused guest region
  base/end, default 0x800B0000..0x80110000, `cfg_str`, hex OK), `DC_ALL` (include the render-only
  packet-pool/OT regions in the report, `cfg_on`).

## Adding a new flag
- A new on/off **diagnostic** → add a `cfg_dbg("yourchan")` call; document the channel in the table above.
  Do NOT add a new `PSXPORT_*_DBG` var.
- A new **config/feature** toggle → `cfg_on("PSXPORT_YOUR_FLAG")`; a valued one → `cfg_int`/`cfg_str`.
- Then list it here. The goal is to keep the count down — prefer a channel over a new variable.

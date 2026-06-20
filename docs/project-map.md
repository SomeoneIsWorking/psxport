# Project map — read this before grepping around

The recurring time-sink in this repo is re-discovering *where things are* and *how to build*.
This is the map. Keep it current when the layout changes.

## Build & run cheat-sheet
| Want | Command | Output |
|------|---------|--------|
| Build+run the **port** (full) | `./run.sh [disc.chd]` | `scratch/bin/tomba2_port` (then runs) |
| Rebuild the **port** (incremental, no run) | `tools/build_port.sh [files… \| all]` | `scratch/bin/tomba2_port` |
| Build the **oracle** (Beetle reference) | `make -C runtime` | `runtime/wide60rt` |
| Drive the port or oracle interactively | `tools/drive.py start native\|oracle` … | — |
| Inspect BGM/libsnd state of a RAM dump | `tools/bgm.py dump <ram>` | — |
| **Disassemble** a MAIN.EXE engine fn (resolves load/store addr + WIDTH) | `tools/disas.py <addr> [--mem]` | — |

- **`make` builds the ORACLE, not the port.** The port has no Makefile (see CLAUDE.md "TWO binaries").
- `tools/build_port.sh` keeps a `scratch/obj/` object cache; one changed file relinks in ~0.5s.
  Its SRC list must mirror `run.sh` step 4 — add new `engine/*.c` or `runtime/recomp/*.c` to **both**.
- `tools/drive.py` does NOT build. Build first, then drive. It sets `PSXPORT_NO_FMV=1`,
  `PSXPORT_BGMDBG=1`, headless (`PSXPORT_NOWINDOW`/`NOAUDIO`) by default; pass `headed` for a window.

> **For WHAT'S PORTED and the execution-order frontier, see `docs/port-progress.md` (the source of truth).**
> This section is the FILE map — what lives where. The two are kept in sync. The codebase is **C++** now
> (`.cpp`); the interpreter core + override ABI stay C-callable.

## `engine/` — Tomba2Engine, the game-specific native engine (one module per SYSTEM)
| file | system | notes |
|------|--------|-------|
| `game_tomba2.cpp` | **override REGISTRY** + misc hooks | `games_tomba2_init()` registers every native override; ov_frame_update, ov_options_menu, asset codecs. The wiring hub — grep here to see what's owned. |
| `engine_init.cpp` | boot init: frame-state / display / camera | `eng_init_framestate/display/camera` (the ov_game_main init prefix). |
| `engine_level.cpp` | stage/level LOADER | `ov_load_stage` (FUN_800450bc), `ov_task0_boot`, `ov_stage_transition`. |
| `engine_stage.cpp` | GAME-stage state machine | prologue + sm[0x48] handlers + running dispatcher + staged ov_game_s4c. |
| `engine_camera.cpp` | per-frame CAMERA follow | `ov_cam_track_xz/y` (position). Matrix builder = TODO. |
| `engine_tomba2.cpp` | object WALK + CULL | `ov_objwalk` (FUN_8007a904), `ov_object_cull`. |
| `engine_submit.cpp` | render SUBMIT + overlay autodetect | geometry submit (GT3/GT4/gt4_bp), per-object render, render-walk. **881 lines — split candidate** (see ORG DEBT). |
| `render_queue.cpp/.h` | engine-owned draw ORDERING | the RenderQueue (layers, re-sort) that replaced the PSX OT read. |
| `native_terrain.cpp` | terrain build + submit | `ov_terrain` (FUN_8002AB5C). |
| `margin_render.cpp` | widescreen margin object render | extra-object collection for 16:9. |
| `fps60.cpp` + `fps60_internal.h` | 60fps interpolation | render-interp layer (parked). |
| `tomba2_types.h` | guest struct field offsets | the entity/node/task layouts. |
| `native_path*.cpp` (9 files) | **LEGACY boot-path transcription** | ⚠️ ORG DEBT — see below. `ov_<hexaddr>` leaves (`native_path`,`_a1..a3`) + non-leaves (`_b1..b5`). Cryptic; to be superseded by named system modules as the top-down port reaches them. |
| `native_dl.cpp/.h` | (near-empty, legacy) | superseded by render_queue; **delete candidate**. |

## `runtime/recomp/` — the PSX→PC PLATFORM (common; future `psxport` submodule)
**Core / glue:** `interp.cpp` (flat R3000 interpreter), `mem.cpp` (bus dispatch + watchpoints PSXPORT_WWATCH/CW),
`core.h`/`game.h` (the `Core`/`Game` objects), `dispatch.cpp` (override table), `hle.cpp` (BIOS HLE),
`threads.cpp`/`timing.cpp` (cooperative threads + timers), `boot.cpp` + `native_stub.cpp` (SCUS entry → MAIN),
`native_boot.cpp` (**boot + the native per-frame loop `native_scheduler_step` + the AUTO_* test-drive
automation + diagnostics — 694 lines, split candidate**), `sync_overrides.cpp`, `watchdog.c`, `stubs.cpp`,
`cfg.c` (the `PSXPORT_*` config + `PSXPORT_DEBUG=chan` channels), `mods.c`.
**GPU/present:** `gpu_native.cpp` (GP0/GP1, VRAM, packet pool — 1544 ln), `gpu_vk.cpp` (Vulkan backend + present —
1746 ln) + `gpu_vk_shaders.h`/`gpu_vk_internal.h`, `gpu_native_internal.h`, `gpu_trace.cpp`/`gpu_debug.cpp`
(GP0 capture/replay = the `gpu_differ`), `imgui_overlay.cpp`.
**Audio:** `spu_beetle.c` (Beetle spu.c mixer lift), `spu_audio.c` (SDL sink + PSXPORT_WAV), `xa_stream.c`
(in-game XA-ADPCM streaming).
**CD/disc:** `cd_override.cpp` (libcd/engine read primitives → native), `cdc_native.c`, `disc.c` (libchdr),
`memcard.cpp`.
**Hardware lifts (vanish when their CALLERS are ported, NOT by re-emulating):** `gte_beetle.cpp` (Beetle gte.c),
`mdec_beetle.c` (mdec.c), `native_fmv.cpp` (STR/MDEC FMV + shared XA decoder), `pad_input.cpp`.

## `runtime/` (root: main.cpp, Makefile, wide60*) — the Beetle ORACLE frontend (NOT the port)
`make -C runtime` builds `runtime/wide60rt`, the reference emulator. Separate from the port; slated to move out.

## Tools, generated, vendor
`tools/` — `recomp/emit.py` (recompiler), `disas.py` (MAIN.EXE disasm), `drive.py` (diff driver), `gpu_differ`,
`build_port.sh`, bgm/frame tooling. `generated/` — recompiled MAIN.EXE `shard_*.c` (gitignored, run.sh rebuilds).
`vendor/beetle-psx` (committed GPL fork), `vendor/imgui`.

## ORGANIZATION conventions + known DEBT
- **One module per SYSTEM, named for the system** (`engine_camera.cpp`, not `native_path_a3.cpp`). New ported
  systems get their own `engine/<system>.cpp`. Name functions `ov_<what_it_does>`, not `ov_<hexaddr>`.
- **Keep files focused; ~400–500 lines is the soft cap for a mixed-responsibility file.** Cohesive
  single-responsibility backends (gpu_vk, gpu_native) may be larger.
- **Known debt (fix incrementally; don't churn working A/B-verified code all at once):**
  1. `engine/native_path*.cpp` (9 files, ~2600 ln) — legacy 1:1 transcription of the boot path with
     `ov_<hexaddr>` names split into arbitrary "batches" a1/a2/b3. SUPERSEDE as the top-down port reaches each
     subsystem: replace a cluster with a named module, delete its entries. Do NOT mass-rename for its own sake.
  2. `engine/engine_submit.cpp` (881) and `runtime/recomp/native_boot.cpp` (694) are crammed — split when next
     touched (submit: geometry-submit vs render-walk vs overlay-autodetect; native_boot: boot vs the AUTO_*
     automation+diagnostics vs the frame loop).
  3. `engine/native_dl.*` is near-empty (superseded by render_queue) — delete when convenient.

## CD path — the part that's easy to get wrong
The port does NOT emulate the CD controller for the game; `cd_override.c` replaces libcd/engine
read primitives with synchronous native disc reads (`disc.c` → libchdr). **There are TWO CD-command
wrappers, and the cutscene XA path uses the second one:**
- `FUN_8008AC34` (libcd `CdControl`) → `ov_cd_command`. Boot/menu uses this.
- `FUN_8001CE90` (the **engine's streaming** CD-command wrapper, used by the streaming reader
  `FUN_8001cfc8`/task) → `ov_cd_cmd_stream`. **In-game cutscene XA-ADPCM streaming goes through
  HERE** (Setmode 0xC8 = Speed+ADPCM+SF-filter, Setfilter file/chan, Setloc, ReadS).
- Both wrappers route the streaming-relevant commands to the native XA engine `xa_stream.c`
  (Setmode/Setfilter/Setloc/ReadS/Pause). Debug both with `PSXPORT_DEBUG=cdcmd` (logs every command
  + params from both wrappers).
- `ov_cd_cmd_stream` also fakes `GetlocL` (cmd 0x10) drive position — the streaming reader polls it.
  See its comment; during XA it must report the **advancing** play position (else the cutscene,
  which waits on the head reaching the clip end, never advances).

### In-game XA-ADPCM audio (`xa_stream.c`) — added 2026-06-16
Decodes CD-XA from the ReadS-streamed sectors and feeds the SPU's CD-audio input via
`CDC_GetCDAudioSample()` (Beetle `spu.c` calls it once per 44.1kHz sample, scaled by the game's
`CDVol` + gated on `SPUControl` bit0 — both game-set). Pull-driven (decode on consumption →
self-paces to realtime). `xa_decode_sector()` lives in `native_fmv.c`. The SPU mixes XA only when
the game enabled CD audio; sequenced (libsnd) BGM is a SEPARATE working path. Debug: `PSXPORT_XA_DBG=1`.

## Rendering, present, and config — see the dedicated docs
- **`docs/render-arch.md`** — the GP0→screen path, SW vs VK rasterizer split, the present dispatch
  (`blit_src`→`gpu_vk_present`), headless **offscreen VK** (`PSXPORT_VK_HEADLESS`), the depth model
  (OT-order default vs `PSXPORT_NATIVE_DEPTH` real per-vertex depth), and the VK render-diff tool
  `tools/vk_depth_diff.sh`. Read before touching graphics/VK.
- **`docs/config.md`** — the `cfg` module (`cfg_on/cfg_int/cfg_str/cfg_dbg`); ALL diagnostics are now the
  single `PSXPORT_DEBUG=chan,chan` var (channel table + old→new map). Don't add raw `getenv("PSXPORT_…")`.
- **`docs/driving-the-game.md`** — how to DRIVE the port to a scene: pad button bits, the automation flags
  (`AUTO_GAMEPLAY`/`AUTO_NEWGAME`/`FORCE_*`), the live debug server (`tools/dbgclient.py`), scene-state
  signals, and the headless-120-frame-cap / `pkill -f` gotchas. Read before driving the game.

## Where state/notes live
- `docs/journal.md` — chronological findings + dead ends (read the head before re-deriving).
- `docs/diff-driver.md` — the drive.py/bgm.py oracle-comparison workflow (committed copy of the skill).
- `<local-notes>/.../memory/MEMORY.md` — cross-session pointers (machine-local; in-repo docs are canonical).
- `scratch/` — gitignored artifacts by type (`bin/ wav/ screenshots/ raw/ logs/ state/`). Never `/tmp`.

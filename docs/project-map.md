# Project map — read this before grepping around

The recurring time-sink in this repo is re-discovering *where things are* and *how to build*.
This is the map. Keep it current when the layout changes.

## Build & run cheat-sheet
| Want | Command | Output |
|------|---------|--------|
| Build+run the **port** (full) | `./run.sh [disc.chd]` | `scratch/bin/tomba2_port` (then runs) |
| Rebuild the **port** (incremental, no run) | `tools/build_port.sh [files… \| all]` | `scratch/bin/tomba2_port` |
| Build the **port** via CMake (IDE/clangd) | `cmake -S . -B build && cmake --build build --target tomba2_port` | `scratch/bin/tomba2_port` |
| Drive the port interactively (REPL) | `PSXPORT_REPL=1 scratch/bin/tomba2_port …` (commands on stdin) | — |
| Inspect BGM/libsnd state of a RAM dump | `tools/bgm.py dump <ram>` | — |
| **Disassemble** a MAIN.EXE engine fn (resolves load/store addr + WIDTH) | `tools/disas.py <addr> [--mem]` | — |

- **There is ONE binary: the native port** `scratch/bin/tomba2_port`. `make` builds nothing now
  (the old oracle Makefile is gone); the port has no Makefile (built by run.sh / build_port.sh).
- `tools/build_port.sh` keeps a `scratch/obj/` object cache; one changed file relinks in ~0.5s.
  Its SRC list must mirror `run.sh` step 4 AND `cmake/tomba2_port.cmake` — add a new `game/*` or
  `runtime/recomp/*` source to **all three** (the three SRC lists are kept in sync by hand).
- **CMake** also builds the port (`cmake/tomba2_port.cmake`, target `tomba2_port`, output still
  `scratch/bin/tomba2_port`): handy for clangd/`compile_commands.json` and IDEs. The shell build
  (run.sh / build_port.sh) stays canonical (run.sh also extracts MAIN.EXE + launches). The CMake
  port target is `-DPSXPORT_BUILD_PORT=ON` by default; it self-skips (warns) if SDL2/Vulkan/FreeType
  dev libs are absent, so the discdump-only configure run.sh uses still works.
- **Self-provisioning:** running `scratch/bin/tomba2_port` directly (no prior `./run.sh`) self-extracts
  `MAIN.EXE` from the disc if missing, resolving the CHD the same way `disc.c` always has (CLI arg >
  `PSXPORT_TOMBA2_DISC`/`PSXPORT_DISC` env > `.env` > a `*.chd` dropped into the working directory).
- **Drive the game with the REPL** (`PSXPORT_REPL=1`, commands piped on stdin), not env vars:
  `run N`, `newgame` (pulse to the GAME prologue), `skip N` (pulse Start N frames into the field),
  `press`/`release`/`tap <btn>`, `r`/`rw`/`w` (memory), `dumpram <path>` (+ `.spad` scratchpad
  sidecar), `shot <path>` (VK-readback PPM, works headless), `debug <chans|all>` (enable diagnostic
  channels at runtime — replaces the `PSXPORT_DEBUG` env var), `stage`, `regs`, `seq`, `quit`.
  Headless render for screenshots: `PSXPORT_VK_HEADLESS=1` (offscreen VK). The live TCP debug server
  (`PSXPORT_DEBUG_SERVER=1`, `tools/dbgclient.py`) has the same commands for a windowed run.

> **For WHAT'S PORTED and the execution-order frontier, see `docs/port-progress.md` (the source of truth).**
> This section is the FILE map — what lives where. The two are kept in sync. The codebase is **C++** now
> (`.cpp`); the interpreter core + override ABI stay C-callable.

## `game/` — the PC-native game, organized by SUBSYSTEM FOLDER (like a PC game engine)
Every subfolder is on the include path, so `#include "foo.h"` resolves regardless of where the header lives.
| folder | subsystem | key files |
|--------|-----------|-----------|
| `game/` (root) | top-level game def + shared types | `game_tomba2.cpp` (`games_tomba2_init()` — the native-wiring hub; grep here for what's owned), `tomba2_types.h` (entity/node/task field offsets). |
| `game/ai/` | per-object BEHAVIORS (AI/logic) | 49 `beh_*.cpp` — one per object handler. The native beh registry lives in `game/object/behavior_dispatch.cpp`; `ents` flags each live object owned/PSX. |
| `game/object/` | object SYSTEM | `object_list.cpp` (object-list walk + field-frame object dispatchers), `behavior_dispatch.cpp`, `array8_dispatch.cpp`, `animation.cpp`, `actor_sm_24448.cpp`, `script_vm.cpp`. |
| `game/world/` | entity lifecycle | `entity.cpp`, `pool.cpp`, `spawn.cpp`, `placement.cpp`, `graphics_bind.cpp`, `object_table.cpp`, `area_slots.cpp`. |
| `game/render/` | rendering | `submit`, `render_frame`, `render_walk`, `projection`, `cull`, `fps60`, `margin_render`, `render_queue`, `lighting`, `native_terrain`, `screen_fade`, `pgxp`; + `render_native.cpp`, `scene_build.cpp`, `mesh_draw.cpp` (folder is FLAT — no `scene/`/`mesh/` subdirs). |
| `game/camera/` | per-frame CAMERA follow | `cutscene_camera.cpp`. |
| `game/scene/` | stage/scene state machines | `demo`, `level_load`, `startup`, `mode_state_arm`, `bg_scene_transition_sm`, `scene_transition`, `scene_events`, `script_interp`, `parallax_bg`, `sop` (intro cutscene). (The GAME-stage sm moved to `game/core/engine.cpp`.) |
| `game/audio/` | sound + music | `sfx.cpp`, `audio_dispatch.cpp`, `music_coord.cpp`, `music_list.cpp`, `native_music.cpp`, `native_audio.c`. |
| `game/input/` | pad input | `input.cpp`. |
| `game/player/` | player physics/collision | `actor_tomba`, `collision`, `hitbox`, `grid_offset`. |
| `game/ui/` | menus / text | `menu`, `font`, `bav_loader`, `save_menu` (class `SaveMenu`). |
| `game/items/` | inventory | `inventory.cpp`. (Save UI → `game/ui/save_menu.cpp`; the script VM → `game/object/script_vm.cpp`.) |
| `game/math/` | math + GTE ops | `gte_math`, `mathlib`, `mtx`, `trig`, `rng`. |
| `game/core/` | engine core + assets | `engine.{cpp,h}` (the `Engine` class + GAME-stage sm), `asset.cpp`, `pc_scheduler.{cpp,h}` (class `PcScheduler`), `verify_harness.{cpp,h}` (class `VerifyHarness` on `Game`). |

## `runtime/ui/` — the overlay's COMPONENT tree
One file pair per component on `psx::ui::Component`, modelled on Dusklight's `src/dusk/ui/` (CC0):
`ui_event` (scoped listeners), `ui_component` (the base + the ONE data->DOM text boundary),
`ui_assets` (asset resolution that refuses to report success), `mod_row_model` (what a row means),
`warp_control`, `render_path_control` (the title-capability-filtered player cycle; PSX stays
diagnostic-only),
`menu_row`, `menu_pane`, `menu_tab_bar`, `menu_readouts`,
`menu_document`.
`runtime/recomp/rmlui_overlay.{h,cpp}` keeps RmlUi's LIFETIME only and knows no elements.
**Read `docs/ui-architecture.md` before adding to the menu** — it carries the shapes taken from
Dusklight, the one place ours deliberately differs, the headless driving surface (`menu dump` /
`menu tab` / `menu nav`), and the decision NOT to stand an ImGui developer stack up yet.

## `runtime/recomp/` — the PSX→PC PLATFORM (common; future `psxport` submodule)
**Core / glue:** `interp.cpp` (flat R3000 interpreter), `mem.cpp` (guest-memory access, top-level
hardware-bus dispatch, and watchpoints PSXPORT_WWATCH/CW), `io_peripherals.{h,cpp}` (SIO0,
I_STAT/I_MASK, and root-counter address decode), `sio_pad.{h,cpp}` (one digital controller on SIO0,
including full-duplex response order, transfer and /ACK deadlines, interrupt request, and port
selection), `cpu_divide.{h,cpp}` (the narrow R3000 DIV/DIVU
quotient/remainder owner shared by emitted and interpreted code),
`game_runtime.{h,cpp}` + `render_capabilities.h` + `guest_program_image.h` +
`guest_pad_buffer_layout.h` + `guest_cd_stream_callback_layout.h` + `game_iface.{h,cpp}`
(derived `GameRuntime` install, one required title declaration for Native/temporal availability,
shared startup/live-selection policy,
the immutable executable-image and direct-runtime guest pad/CD-stream callback fact owners, per-Game
driver/scheduler and optional temporal-presentation factories, and the bounded legacy projection),
`legacy_game_config.h` / `legacy_game_hooks.h` (the deprecated data and callback bags kept
source-compatible while consumers migrate), `core.h`/`game.h` (the `Core`/`Game` objects;
`Game` owns the runtime-created products), `dispatch.cpp` (override table), `hle.cpp` (BIOS HLE),
`GameRuntime::guestVramIsPicture(const Game&)` (the required inherited current-frame authority for
whether guest VRAM is picture content beneath native geometry; the checked renderer query refuses a
missing runtime and `gpu_vk.cpp` never reads the legacy static backdrop bit),
`guest_vram_composite_policy.h` (the per-`Game` persistent-composite ownership latch; both policy
transitions invalidate the old composite and native-to-guest requests a complete VRAM upload),
`gpu_vk_present_policy.h::preserve_composite_backdrop` (the PC-composite persistence rule: stable Gte
ownership preserves both guest framebuffer pages, its first ownership build initializes them, Native
clears rebuilt frames, and Psx/guest-VRAM picture ownership remains independent),
`game_hooks_opt.{h,cpp}` (the guarded boundary for neutral optional compatibility callbacks, including
the absent-table zero/no-fade presentation state); temporal-only guarded callbacks live separately in
`fps60_game_hooks.{h,cpp}` so a direct runtime does not link them,
`bios_interrupt.{h,cpp}` (the HookEntryInt saved-context contract),
`syscall_exception.{h,cpp}` (the R3000A syscall Cause/EPC and Status-stack transition shared by the
emitter, interpreter, and native HLE; `rec_dispatch_miss` owns pre-HLE observation of external BIOS
targets while generated entries retain their emitted checkpoint owner),
`threads.cpp`/`timing.cpp` (cooperative threads + host-advanced display time),
`frame_loop_shell.{h,cpp}` (mandatory title-driver preflight, exactly-one-frame delegation, and
mechanical exactly-one-presentation-fence enforcement),
`boot.cpp` + `native_stub.cpp` (SCUS entry → MAIN),
`native_boot.cpp` (boot + host loop scaffolding + diagnostics; `dc_step_frame` delegates through
`FrameLoopShell` and contains no title frame body or fallback; `repl.cpp`/`repl.h` own only generic
interactive command transport and framework diagnostics, while `GameRuntime::replCommand` owns every
title-specific command and the legacy adapter forwards that boundary without assuming a callback),
`platform_hle.h` + `sync_overrides.cpp` (the guarded SCEI-library HLE table; direct
`PlatformHlePlan` consumers supply typed addresses for framework-owned standard leaves such as
SetGeomOffset/SetGeomScreen and stock `CdRead`/`CdReadSync`, plus the mandatory measured
`vsyncAddress`; both direct and adapter
runtimes bind that address to one non-replaceable all-mode abort, while explicit `{addr, fn}` rows are
only for other title-specific sync behavior; legacy consumers retain the corresponding address facts
through `GameConfig::hle`. Direct and legacy exact half-open address-window storage and validation use
the one `kPlatformHleWindowCapacity` constant), `watchdog.cpp`
(SCEA/FMV image progress retains cold-init grace until the first main-VRAM presentation completes; every later
heartbeat uses steady timing), `stubs.cpp`,
`cfg.c` (the `PSXPORT_*` config + `PSXPORT_DEBUG=chan` channels), `mods.c`.
`synchronous_task_wait.{h,cpp}` owns the one product policy for a cooperative spawn-and-wait: game-specific
task addresses and continuation PCs come from `GameConfig`, the spawned task is pumped to its authored close,
and completion returns without manufacturing loading frames. `pc_scheduler.cpp` delegates to that owner; the
generated multi-frame routine remains an explicit oracle rather than a second product launch mode.
`bios_libc_string.{h,cpp}` owns Sony libc's string/character/memory-compare leaves behind the narrow dispatch called by
`hle.cpp`. It implements `A0:0x15` (`strcat`) as a guest-address byte loop: it scans the
destination, copies the source through `Core::mem_r8`/`mem_w8` including the terminator, and returns
the original destination. `A0:0x1A` (`memcmp`) compares guest bytes in ascending order and returns the
signed difference of the first unequal unsigned-byte pair; zero length reads nothing. CTR first
falsified the missing leaf live at caller RA `0x8001C5D4` with a three-byte startup comparison.
`test_bios_libc_string` reaches the shipping dispatch seam and gates the
return value, terminating write and surrounding bytes, KSEG aliasing, forward guest alias copy order,
empty inputs, `memcmp` equality/positive/negative/alias/zero-length behavior, and opposite answers for
a wrong table and the unimplemented neighboring leaf. The same
shipping seam owns locale-independent ASCII `A0:0x25` (`toupper`); its controls prove lowercase
conversion while uppercase, digits, and `0xE0` remain unchanged.
`hle.cpp` also implements Sony libc `A0:0x2F/0x30` (`rand`/`srand`) with per-`Hle` state and the exact
32-bit LCG (`state*0x41C64E6D+0x3039`, return `(state>>16)&0x7FFF`). It deliberately does not call
host `rand()`: host sequences differ and process-global state would couple SBS/dual-core Games.
`test_bios_rand` reaches the shipping `Hle::dispatchBios` seam and gates the exact seed-1 sequence,
restart behavior, per-Game isolation, and negative wrong-table plus neighboring-function cases.
`Hle::workAreaInit` owns the guest-visible B0/C0 work tables returned by `B(57h)`/`B(56h)`. They live
in ordinary `Core` main RAM, so guest rewrites remain authoritative after initialization. The C0
table preserves its existing `jr ra; nop` fallback at slots 0/1 and publishes slot 6 as
`0x00000C80`, the `C(06h)` ExceptionHandler entry address. `test_bios_work_area` drives the shipping
`Hle::dispatchBios` B56 boundary, checks both adjacent-zero answers, and proves a guest table rewrite
and the handler words at `0x00000C80` survive a repeated B56 call.
`Hle::irqPoll` walks the measured SysEnqIntRP element chain and then enters the optional custom
exception exit installed by `B0:0x19 HookEntryInt`. `bios_interrupt.{h,cpp}` owns the jmp-buffer
layout (`ra/sp/fp/s0..s7/gp`) and makes the saved setjmp continuation return non-zero.
`B0:0x17 ReturnFromException` raises a private scoped unwind through generated C, so the continuation
cannot fall through into its one-time initialization; only then does the outer injection restore the
interrupted `R3000`. Generated wrappers acquire their diagnostic attribution owner through
`InterpDiag::OtAttrScope`, so that unwind releases every crossed wrapper without an IRQ-specific repair
path. `B0:0x18 ResetEntryInt` clears the context. `test_bios_interrupt` drives the shipping BIOS entry
points and the full `irqPoll -> rec_dispatch -> B0:0x17` seam, proving CPU and diagnostic-owner context
restoration, zero-buffer/zero-RA refusal, non-returning unwind, and the illegal normal-return answer. An
emitter regression separately requires the generated wrapper to use the shared scope. A consumer must
seed the measured saved RA as
`main_reentry`, because it is usually inside the interrupt bootstrap rather than a natural function
entry.
`dma_callbacks.{h,cpp}` owns direct-runtime Sony `DMACallback` registrations as typed, per-`Game`
host state. A title-native `DMACallback` body exchanges one channel entry there and retains ownership
of its measured DICR update; `Hle::irqPoll` resolves this registry only when `Core::cfg` is null, while
legacy adapters continue reading their measured guest callback table. `test_direct_dma_callbacks`
drives the shipping DMA3 completion and interrupt-delivery path, proving previous-callback return,
single delivery, register preservation, and the no-registration result.
The runtime seam is partial: shipping consumers inherit `LegacyGameRuntimeAdapter` until typed fact
groups replace every generic `c->cfg` read. `GuestProgramImage` is the first landed group: derived
runtimes own crt0, resident MAIN routing, and backtrace-code facts; `Core` snapshots that immutable view,
and those algorithms no longer read `GameConfig`. `GuestPadBufferLayout` is another landed group:
direct runtimes declare the measured Sony receive buffers while `Pad` retains config-first resolution
for adapters. The adapter's one-way projection keeps unmigrated consumers source-compatible.
`OtAttr` accepts either the legacy fixed `packetPoolBase/Stride` pair or two measured guest
`packetPoolBasePtrs/EndPtrs` descriptor pairs for heap-allocated parity pools. It caches live bounds and
invalidates them on descriptor writes; the pools remain separate so an allocation gap is never treated
as render memory.
`DiscIdentity` is the next candidate group; the consumer follow-up and deletion set live in
`docs/plans/game-seam-redesign.md`.
`overlay_router.{h,cpp}` owns both live-range relocatable modules and signature-identified fixed
modules. Fixed ranges may nest: the router evaluates every containing resident identity (a current RAM
signature, or a loader-recorded slot identity after the game mutates its header), chooses the smallest
range, and refuses equal-specificity ambiguity. Dispatch, entry validation, and resident-name
diagnostics share that one resolver. Its recdep histogram remains per-Core; `Game::~Game` invokes the
router's dump before any `Game` member is destroyed, so the diagnostic never retains a Core or
PlatformHle pointer past its owner. `test_overlay_reloc` includes Crash Bash's measured BOOT/MENU
nested ranges and signatures, reverses and renames their registry, removes the nested signature where
no loader identity exists, and constructs an equal-width ambiguity; the real consumer now routes
MENU's 0x800B5244 target instead of first-matching BOOT.
`ot_attr.{h,cpp}` owns the logic-frame stamp contract: pre-loop boot stores are counted, and the
run-end report distinguishes satisfied, failed, and unexercised rather than warning before a loop can start.
**GPU/present:** `frame_presenter.{h,cpp}` owns non-temporal current-frame capture, one real present,
diagnostics, explicit field pacing, and ledger reconciliation. Its `commitUnpresented` entry rotates the
same fence, ledger, and capture state for the diff-mode field path without emitting, presenting, pacing,
or recording a diagnostic; the injected-backend test proves both that path and the next visible commit.
`frame_dump_window.h` owns the pure late-start predicate used by the capped per-present diagnostic, so
long boot routes can select a target fence without changing presentation or consuming the file budget.
`Fps60` is an optional temporal decorator, not the frame-lifecycle owner. Direct runtimes declare
their presentation products through `RenderCapabilities`; unsupported fps60 requests and UI bindings
are refused before reaching the decorator. The same capabilities own the title's face-order factory
default: `Mods::init` applies it before loading `psxport_settings.ini`, so a title can ship faithful
frame-wide authored OT order without overriding an explicit persisted player choice. The generic
factory remains PC-native per-pixel depth. `fps60_gpu_present.{h,cpp}` owns the intermediate-pass renderer reset and is
referenced only by `fps60.cpp`; the neutral presenter has no temporal renderer operation.
`guest_widescreen_projection.h`
owns the typed, frame-latched title projection/presentation plan; the GTE-only positive contract remains
separate from Native-only `RenderMode::enhancementsAllowed()`. `gpu_display_mode.h` is the pure GP1(08h)
horizontal decoder, including bit 6's 368-dot mode. Full ownership and consumer rules are in
`docs/presentation-contract.md`. `gpu_native.cpp` (GP0/GP1, VRAM, packet pool — 4,121 ln), `gpu_vk.cpp` (SDL_GPU backend +
present — 4,404 lines), `gpu_primitive_dump.{h,cpp}` (primitive-census CSV lifecycle and row encoding),
and `image_writer.{h,cpp}` (the checked RGB24-to-PPM/PNG host-file boundary shared by software and GPU
captures). The two legacy renderer files remain critical extraction territory and are shrink-only.
`render_queue.{h,cpp}` + `painter_object_layer.{h,cpp}` own atomic painter admission and command planning;
`RenderQueue::preflightPainterObjectBatch` admits a complete multi-object publication without mutating
the queue, so callers can validate shared transactions before publishing any object.
Legacy painter objects retain one sequence-stable local stream across textured/untextured and
opaque/semitransparent material variants. Authored replay domains merge several producer objects into one
guest OT stream with the single `(ot_bin descending, link_ordinal descending, chain_suborder ascending)`
comparator; duplicate keys, unordered world faces, and mixed policies refuse instead of guessing. Physical
`RenderQueue::flush` epochs remain independent because a captured logic frame may contain several complete
OT traversals. A consumed queue retains storage only for the next push's lazy reset; it is not a pending
physical epoch, so an empty later flush has no sort, diagnostic, ledger, capture, or emit side effect.
Both neutral presentation and `Fps60::presentPass` call the same pointer-stream planner, so
presentation cannot bypass painter ordering or copy multi-megabyte `RqItem` payloads.
`gpu_painter.{h,cpp}` owns painter target lifecycle, command staging, and the focused real-GPU
discriminator for the custom untextured painter. Both painter fragment paths enforce each item's
inclusive PSX draw area before shading; this prevents valid authored geometry from leaking into
guest guard rows when the host target is wider than the original viewport.
`ot_lifo_depth.{h,cpp}` encodes PSX `AddPrim` head-insertion order for equal-key authored faces, while
`gpu_vk_next_distinct_3d_depth` owns conversion to raster-distinct Vulkan D32 values. `gpu_vk.cpp`
retains interleaved textured and untextured command runs (including explicit flat/Gouraud and DTD state).
Each object seeds its reusable packed target from the current canvas, replays opaque commands directly,
and runs decode -> fixed-function PSX blend -> encode for every semitransparent command so each blend sees
the immediately preceding authored result at 5-bit precision. The resolved packed color + real D32 then
depth-composite into the world. `gpu_vk_semi_selftest.cpp`
owns the 16-case PSX semi-textured equation matrix, packet setup, integer reference equations, and verdict;
`gpu_vk_texture_phase_selftest.cpp` separately owns the 28-case 1x/3x opaque/semi integer-pixel UV-phase
matrix, including positive and negative fractional slopes that distinguish PSX round-to-nearest from
truncation. `gpu_vk_untextured_selftest.cpp` owns the ordinary opaque G3 discriminator for PSX
integer-coordinate edge coverage, 8-bit interpolation rounding, 5-bit truncation, and both signs of
the DTD matrix. `gpu_vk_modulation_selftest.cpp` owns the texture*color modulation discriminator: a
dark texel whose additive, average, and opaque cases separate the PSX's truncating `(texel5*color8)/128`
from round-to-nearest, each seeded so a case that never rasterized cannot read as a pass.
`shaders_gpu/tri.vert` owns the half-pixel transform that aligns those PSX integer
sample coordinates with Vulkan pixel centers; the textured pipeline retains its separately verified
phase. `shaders_gpu/tritex.frag` and `shaders_gpu/trisemi_hw.frag` own that integer modulation. All four use
`gpu_vk_selftest_support.h`; `Game` construction makes the selftest reachable from
every port, and `gpu_vk.cpp` supplies only the shipping
upload/`render_geom`/readback operation. `shaders_gpu/psx_uv.glsl` is the one 12-fractional-bit
integer-native-pixel reconstruction shared by opaque, semi, and semi-cover fragment shaders. The remaining
shipping GPU selftest reads both local and post-composite D32 boundaries. The untextured companion pipeline
applies the PSX 4x4 dither matrix in native-pixel coordinates only for Gouraud+DTD.
`wide_margin_plan.h` (renderer-only coverage for host-visible VRAM extension),
`gpu_vk_internal.h`, `gpu_native_internal.h`, `gpu_debug.cpp`. `test_image_writer` reaches the shipping
file boundary and gates invalid-input refusal, parent-directory creation, the PPM header, and all six
payload bytes. `cmake/gpu_shaders.cmake` owns the
per-consumer build-tree `psxport_generated/gpu_vk_shaders.h`; no generated shader header is shared
through the source tree.
**Independent GPU diagnostic:** `gpu_beetle.cpp` tees GP0/GP1, native image uploads, and GPUREAD drains
into Beetle's software GPU without advancing its CPU/scanout clock; `GPU_StartFrame` is called only at
guest-frame boundaries. The census in `psxport_gpu_census.h` owns accepted/dropped/dispatched/known-no-op/
loss denominators. This compares rasterizers given one command stream; it is not a whole-machine oracle.
**Interpreter comparison harness:** `sbs.cpp` constructs two independent `Game`/`Core` instances. The A
leg may use native overrides; the B leg executes the guest bodies through the interpreter and software
GPU. `dc_boot_init` initializes CD and `PlatformHle` service tables per `Game`, so B cannot inherit a
healthy-looking process-global override from A. `GameHooks::devWarp` is one complete game-owned cold
warp used by both the standalone REPL and SBS; the framework owns timing, not guest addresses or area
machine layout.
`native_diff.{h,cpp}` is the narrower per-call owner oracle. Every active `ndiff_run` has an independent
snapshot frame, so a natively owned parent may call a separately owned child without the child replacing
the parent's pre-state or native answer. `test_native_diff` reaches the shipping API: an equivalent nested
parent/child produces zero divergences, while an independently mutated child produces two real child
divergences (one in each parent leg) and leaves the equivalent parent itself matched.
**Interpolation camera seam:** `fps60.cpp` owns camera capture/lerp and reaches temporal-only guarded
callbacks through `fps60_game_hooks.{h,cpp}`. `GameHooks::fps60ReadSceneCam` remains game-owned; Tomba! 2
decodes its scratchpad matrix in `game/core/game_hooks.cpp`. A missing reader aborts when a native
projection path asks for it—there is no framework camera-address fallback.
`GameHooks::fps60TemporalRotate` is the separate post-two-present lifecycle seam for game-owned immutable
render recipes; it is optional and does not alias the transitional billboard-history hook.
**Audio:** `spu_beetle.cpp` (Beetle spu.c mixer lift), `spu_audio.cpp` (SDL sink +
`PSXPORT_WAV` + opt-in `audiofield` per-field trace), `audio_field_report.{h,cpp}` (exact PCM/cadence/XA
reports), and `sbs_audio_compare.{h,cpp}` (opt-in SBS per-field oracle comparison),
`spu_field_cadence.h` (exact display-field-rate → SPU-clock/sample schedule), and
`xa_stream.cpp` (in-game XA-ADPCM streaming). The sink reads the same exact NTSC 60,000/1,001 or
PAL 50/1 rational owned by `field_rate.h`; it carries integer remainders between fields instead of
rounding every NTSC field to the exact-60 Hz legacy values of 564,480 clocks and 735 samples.
`test_spu_field_cadence` exercises the shipping accumulator at exact 60 Hz, NTSC, and an NTSC→PAL
rate change; 60,000 NTSC fields total exactly 44,144,100 stereo frames (44.1 kHz for 1,001 seconds).
**CD/disc:** `cd_override.cpp` (libcd/engine read primitives → native), `cd_control.h` (public,
game-validated blocking-control seam plus direct-runtime binding targets for the shared synchronous
stock `CdSync`/`CdRead`/`CdReadSync` owners and the typed native finite-read ownership query),
`guest_cd_stream_callback_layout.h` (the typed direct-runtime
guest-RAM slot containing the current CD-ready callback; `Cd::pumpStream` shares this with legacy
`GameConfig::cdReadyCbPtr` consumers and dispatches it only while the controller reports a current
INT1 data-ready response), `cdc_native.cpp` (per-Game register/FIFO/IRQ model, BFRD
latch and command effects), `cdc_command_phase.{h,cpp}` (oracle-derived command receive, argument,
execution, and completion scheduler), `cd_drive_timing.cpp` (nominal 75/150-sector thresholds),
`emulated_time.{h,cpp}` + `timing.cpp` (per-Game deterministic emulated CPU-time owner, exact
fractional-display-field VBlank delivery, the NTSC/PAL HBlank phase exposed through root counter 1
at `0x1F801110`, and timer-2 stopwatch value/mode/target registers at `0x1F801120..128`),
`frame_pacer.{h,cpp}` (display cadence + guest field delivery + optional host sleep), `disc.cpp`
(libchdr plus the parsed CHD track layout and Sub-Q position synthesis), `memcard.cpp`. Native CDC
command `0x11` (`GetlocP`) returns all eight BCD track/index/relative/absolute position bytes from
that layout; a missing disc position is an `INT5` error, never a successful all-zero position.
`test_cdc_getlocp` exercises the shipping command path, metadata1/metadata2 parsing, multi-track
selection, and malformed metadata refusal. Generated blocks and the oracle interpreter advance the same deterministic
clock; a shared display-field delivery advances it to the guest-programmed NTSC/PAL boundary even
when host sleeping is disabled. ReadN schedules its first and following INT1 at nominal 451,584 ticks
(1x) or 225,792 ticks (Setmode bit 0x80, 2x). One instruction currently contributes one tick, so this
is deterministic ordering rather than cycle-accurate physical timing.
Root counter 1 remains the read-only guest observation of the free-running low-16-bit HSync count.
Root counter 2 supports system-clock and clock/8 stopwatches, its four timer-2 sync modes, and
bit-3 reset at the programmed target period; its target/wrap IRQ and reached-target/reached-max
status bits remain deliberately unmodelled.
No Sony libetc `VSync` mode is a shipping observation or field-delivery seam: every product declares
its measured address and all modes abort there. `test_hsync_counter` gates the MMIO observation,
intra-field line-248 progression, NTSC/PAL field geometry, and invalid-cadence refusal;
`test_root_counter2` gates the shipping MMIO path, clock sources, sync-mode stop/free matrix, writes,
wrap, target storage, and the measured latch/spin timeout. `test_vblank_irq` gates full and fractional
field cadence plus masked/later-unmasked SysEnq chain delivery. `test_sio_pad` gates the shipping SIO0
MMIO response sequence, absent devices/port 2, transfer and /ACK timing, interrupt gating/re-arming,
and the measured JOY_STAT pulse/CTRL acknowledge order. `test_vsync_ownership` gates direct and adapter
trap installation, modes -1/0/1/N, replacement refusal,
missing-address refusal, and address-window refusal. The clock uses nominal non-interlaced field
geometry; alternating interlaced field parity is not modeled.
BFRD never creates an event:
reasserting it preserves a partial DMA cursor, and a later transition only installs a sector whose
drive deadline already elapsed. Command writes arm a 12,315-tick receive deadline; arguments transfer
at 1,815 ticks each, execution follows 8,500 ticks later, and only then are side effects and INT3
made visible. Multi-phase commands hold their later INT2 until the current IRQ is acknowledged, and
an exact drive/command deadline tie services drive INT1 first. Pause/Stop cancels the owned deadline.
`test_cdc_command_phases` gates zero/three-argument timing, late side effects, argument validation,
INT3/INT2 separation, replacement, and tie ordering. `test_cdc_emulated_time`
gates instruction-heavy versus yield-heavy deadline delivery, fractional subfields, late-boundary
resynchronization, and invalid cadence. `test_cdc_bfrd_split_dma`
gates latch/access behavior (535 checks), `test_cdc_continuous_read` gates too-early/due, partial,
Pause, full-drain, status and speed answers (64 checks), and `test_interp_guest_cycles` plus the
emitter execution suite gate interpreted/emitted clock advancement. DMA3 commits its programmed
word count: FIFO words first, then the controller's zero read value after depletion; it never
preserves stale destination RAM or consumes a future sector. `test_cdc_dma_depletion` gates the
shipping CDC and Core DMA3 paths with the measured 504-word/70-word-tail split, including deasserted
BFRD (3 cases, 1,518 checks).
Crash Bash's pre-landing one-shot consumer trace crosses its former GetTN empty-poll boundary and
advances through Setloc, Setmode, ReadN, and Pause into later continuous sector ranges; that evidence
validates the command handoff, not a first frame or gameplay.
**Hardware lifts (vanish when their CALLERS are ported, NOT by re-emulating):** `gte_beetle.cpp` (Beetle
gte.c). `gte_state.h::GTE_ExecuteIsolated` runs any vendor GTE instruction against an explicit `GteRegs`
without changing the caller's bound state. Its implementation tracks nested isolation depth and suppresses
PGXP/diagnostic callbacks. The vendor binding and FLAGS accumulator are thread-local. `test_gte_isolated`
differentially gates INTPL, NCDS, and DPCS against the ordinary bound path, verifies caller restoration,
sequential state interleaving, and simultaneous two-thread TLS isolation, and includes a forced mismatch
discriminator. Callback suppression and nested entry are implementation properties, not dynamically tested
by that suite. `native_projection.{h,cpp}` is the producer-facing pure endpoint projection seam: typed
fixed affine, projection parameters (including the RTPS depth-cue inputs), and model vertex in; exact
integer IR/SZ/SXY/FLAG plus pre-saturation raw view coordinates out. It has no Core/GTE/ambient state and
is intentionally not a temporal recipe. The
framework's legacy projection probe delegates to the same implementation through a private sf/lm adapter;
`test_native_projection` differentially covers the producer mode plus the probe's sf=0/lm=1 modes against
isolated vendor RTPS, including all hardware FLAG contributors, saturation, a zero-FLAG control, and
forced endpoint/FLAG mismatch discriminators. `mdec_beetle.c` (mdec.c),
`native_fmv.cpp` (STR/MDEC FMV + shared XA decoder; direct movie presents report watchdog progress),
`pad_input.cpp` (final effective mask + shared `ActiveLowEdges`; the standard packet writer resolves
legacy config first or the direct runtime's typed `GuestPadBufferLayout`, while a missing layout writes
no invented guest address; game/sequence code owns every resulting transition; slot 1 remains absent
by default and a title with measured two-slot guest handling opts in explicitly).

## `tools/port/` — portable game-launch policy

`launch_environment.py` owns the process-environment split between the shipping player and agent
automation. A player product exec is explicitly windowed, audible, and paced even when its parent
environment contains agent-only runtime flags. An agent run explicitly requests headless rendering,
no audio device, and no pacing; window visibility never decides pacing.

## Tools — ONE LINE EACH, and what is wrong with this list

**32 tool files here; 172 across the workspace.** USER, 2026-08-13: *"I think you made the project too
bloated, hordes of tools and tests that no one knows what they are for"*. That is accurate, and this table
exists so the claim can be checked rather than argued. Two rules follow from it:

1. **A new tool needs a row here, or it does not land.** Prefer a new MODE of an existing tool over a new
   file. A tool that checks another tool is almost never worth a file — `prove_crossvalidate_discriminates.py`
   was deleted the day it was written for exactly that reason (a proof of a proof), its one-time result
   kept in `docs/findings/oracle-crt0-crossvalidation.md` where it belongs.
2. **A finished one-off gets DELETED, not documented.** `restructure.py` went when the
   `native_path*.cpp` files it re-sorted stopped existing. No tombstones.

| tool | what it is for | note |
|---|---|---|
| `recomp/emit.py` | the static recompiler: PSX MIPS -> emitted C; owns binary-derived function discovery for resident MAIN and loaded modules, then emits deterministic guest instruction-time | `test_emit.py` drives the shipping CLI to prove unreferenced MAIN handlers after `jr ra` become dispatchable without manual seeds; the same `emit_module` seam gates false early-return merging, switch-case pruning, prologue preservation, `main_reentry`, trampoline tables, GPR-clobber tracking, target alignment, overlay-data rejection, and path-sensitive instruction counts; `test_decode.py` rejects non-canonical COP2 moves with reserved low bits; `test_interp_guest_cycles` matches the interpreter on a four-instruction window |
| `recomp/psexe.py` | load PS-X EXEs and raw RAM images for framework RE tools | ownership test covers success and refusal paths |
| `abi_extract.py` | static ABI/stack-contract extractor for generated function bodies | |
| `port_check.py` | equivalence gate: does a native port's guest-visible store sequence match the substrate | |
| `port_gen.py` | first-draft generator for a byte-faithful native class method | |
| `logsig.py` | extract the message template of every diagnostic call site | selftest PASSES |
| `layout_move.py` | the planned `runtime/recomp/` -> `runtime/<subsystem>/` move | selftest PASSES; move NOT done yet |
| `gen_gpu_shaders.py` | compile the fixed SDL_GPU shader set and shared includes into each consumer build's `psxport_generated/gpu_vk_shaders.h` | 7/7 selftest; `test_gpu_shader_build_ownership.py` 4/4 compiles nested `add_subdirectory` consumers, survives peer clean, rejects a missing target include owner, and reproduces the legacy shared-BYPRODUCT failure |
| `tool_selftests.py` | run every tool's `--selftest` in a repo, and name the ones with none | in `scripts/` |
| `exe_similarity.py` | address-independent code similarity between PS-EXE images | needs 2 executables |
| `lineage_probe.py` | whole-function + string lineage evidence between PS-EXE images | selftest needs a corpus |
| `disasm.py` | disassemble a region of a 2 MB main-RAM dump (capstone) | |
| `dbgclient.py` | REPL client for the debug server | needs a live server |
| `ghidra_decomp.py` · `symdump_re.py` · `symwidth_re.py` | Ghidra headless scripts | run only inside Ghidra |
| `oracle/oracle_trace` | run a real executable in the independent reference emulator; trace instructions, capture canonical ordinal-call or exact pre-execution PC plus I_STAT/I_MASK/DPCR boundaries, resume through explicit ordered syscall/BIOS models, seed explicit consumer-owned main-RAM words at the BIOS stage, and refuse wrong order or sticky hardware/event-tainted continuation | + `oracle_spike` (84-check CPU/resume/IRQ/DPCR/event/CP0 gate), `test_oracle_trace.py` (17-answer CLI gate: distinct ordinals, indirect `jalr` target, exact CPU/device blocks, single and ordered modeled returns, alternate RAM seed, wrong order, unreachable and hardware-tainted refusals), `crossvalidate_crt0.py --selftest` |
| `crt0_extract` | report a PS-X EXE's crt0 boot group through the shipping decoder | |
| `discdump` | extract files from a CHD/ISO without `run.sh` | |
| `smoke/psxport_smoke` | the agnosticism proof: link libpsxport against a stub, zero game symbols | |

**GAME-SPECIFIC tools sitting in the game-agnostic framework** — they belong in `Tomba2Engine/tools/`, and
their presence here is the same defect as the guest addresses in `runtime/` (see
`docs/findings/framework-carries-game-addresses.md`):

| tool | why it is not framework |
|---|---|
| `disas.py` | "MIPS-I disassembler for **Tomba!2's MAIN.EXE**" |
| `yield_reach.py` | asks whether a function reaches `FUN_80051f80` — a Tomba!2 guest address |
| `frame_audit.py` · `producer_class.py` | audit Tomba!2 native overrides / graphics producers |
| `interp_dump.py` | a wide60 proof-of-concept over a GPU dump |

**PURPOSE UNDOCUMENTED — nobody can say what these are for.** They have no docstring and no row anyone
wrote. Each is either given a one-line purpose or deleted; being listed here is not the same as being
justified: `ldscan.py`, `prof_report.py`, `symres.py`, `vramcmp.py`, `vram_png.py`.

`generated/` — the recompiled substrate (gitignored). `vendor/beetle-psx` (committed GPL fork: the port's
GTE/MDEC/SPU/CHD **hardware backend** AND, as of 2026-08-13, the independent reference emulator behind
`tools/oracle/`), `vendor/rmlui` (the overlay's HTML/CSS engine), `vendor/lucent` (the logger).
**`vendor/imgui` is VENDORED BUT DEAD** — 2.9 MB of committed source referenced by no build file and no
source file (measured 2026-08-06), not even a submodule. `docs/ui-architecture.md` has the decision not to
stand an ImGui developer stack up yet, and the one-line removal if that holds.

## ORGANIZATION conventions + known DEBT
- **A native belongs in its SUBSYSTEM FOLDER, named for the system** (`game/camera/cutscene_camera.cpp`), one
  cohesive responsibility per file. Name functions `ov_<what_it_does>`, not `ov_<hexaddr>`. NEVER a
  general-purpose grab-bag file, NEVER cram unrelated subsystems into one file (the old flat `engine/` and
  `native_path*.cpp` are GONE — do not recreate them).
- **Keep files focused; ~400–500 lines is the soft cap for a mixed-responsibility file.** Cohesive
  single-responsibility backends (gpu_vk, gpu_native) may be larger.
- **No grab-bag files** (verified: no `*misc*`/`*util*`/`*common*`/`native_path*`/`native_dl*` anywhere). The
  two dead `*misc*` grab-bags were deleted (later-288, proven 100% dead + 0-diff), and the REPL driver was
  extracted from `native_boot.cpp` into `runtime/recomp/repl.cpp` (+ `repl.h`, and shared `guest_call.h` for
  the rc0-4 dispatch helpers). If you catch yourself creating a `misc`/`util` dumping ground, STOP — put each
  native in its subsystem file.
- **Remaining size debt:** `native_boot.cpp` still holds boot + the native per-frame scheduler.
  `gpu_native.cpp`/`gpu_vk.cpp`/`game/render/submit.cpp` exceed the default 1,200-line source cap; treat
  each touched responsibility as an extraction boundary. The renderer caps ratcheted to 4,121 and 4,404
  after moving primitive-dump diagnostics and image-file output to peer owners; do not grow them again.

## CD path — the part that's easy to get wrong
The port does NOT emulate the CD controller for the game; `cd_override.cpp` replaces libcd/engine
read primitives with synchronous native disc reads (`disc.c` → libchdr). **There are TWO CD-command
wrappers, and the cutscene XA path uses the second one:**

`cd_control_sync(Core*)` is the narrow public seam for a game-owned blocking libds wrapper. The game
must validate its command class before calling it. The helper applies the existing synchronous
`cd_command` effects, zeroes a non-null result buffer through that path, and changes the return value
to blocking-control success (`V0 = 1`); it does not claim query, read, callback, or game-specific
wrapper semantics.

`cd_sync_stock_sync(Core*)` is the single synchronous Sony `CdSync` owner for direct runtimes: it
reports ready (`V0 = 2`) and clears the optional result bytes without entering the guest library's
VSync timeout loop. Direct runtimes declare stock finite-read ownership through
`PlatformHlePlan::cdReadAddress`/`cdReadSyncAddress`; this same typed fact prevents a subsequent
continuous ReadN/ReadS from entering the callback-driven finite-read burst. Continuous STR/XA
readers publish `GuestCdStreamCallbackLayout` from a direct
runtime (or retain `GameConfig::cdReadyCbPtr` while migrating); `Cd::pumpStream` reads the live guest
function value from that slot and delivers only the drive-paced sector budget.

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
the game enabled CD audio; sequenced (libsnd) BGM is a SEPARATE working path. Push-mode controller
routing mirrors Beetle's `PS_CDC_XA_Test`: when MODE_SF is set, only the programmed file/channel is
decoded, while non-selected audio remains in the controller data path. The hermetic
`test_cdc_xa_filter` regression covers both filtered and unfiltered modes. Debug: `PSXPORT_DEBUG=xa`
(add `xasec` for a line per sector).

## Tests — `ctest`, the framework gate (`tests/`)

psxport is shared by the game ports, so a framework change is gated by a **hermetic** suite that
needs no disc and no window — the games each need a disc to run at all, so anything
disc-gated is useless as a shared gate. A standalone configure owns that complete suite through
`PSXPORT_BUILD_TESTS=ON` (the default when psxport is the top-level project). An embedded consumer
owns its own CTest surface and defaults the option OFF: framework targets remain available, but psxport
does not advertise test executables the consumer target did not build or run framework style policy
against the consumer tree. A consumer can explicitly opt into the complete framework suite.

`test_cmake_test_ownership.py` configures a real embedded fixture twice and proves both sides of that
boundary: the default registers only the consumer's probe, while explicit opt-in registers the framework
oracle, runtime, loader, and policy gates. This prevents stale binaries in a reused consumer build from
masquerading as current framework failures.

`test_game_runtime` proves a derived runtime is the installed authority, owns per-Core context lifecycle,
creates exactly one `FrameDriver` and `TaskScheduler` per `Game`, and that the bounded legacy pair still
delegates while consumers migrate. It also proves direct and adapter runtimes expose the same typed
program-image view. `test_guest_program_image_ownership` mechanically rejects any crt0/router/backtrace
consumer that reaches back into the legacy config or lets `GameHooks` own executable facts.
`test_frame_loop_shell` proves the shared product preflight refuses a missing driver, reinstalls the
fatal VSync trap after a title displaces the generated override, and then delegates each product frame
exactly once to its title-owned finite driver before checking the one-fence invariant. A shell step
that bypasses preflight aborts before invoking the driver.
`psxport_smoke` derives the new seam with both legacy views null, so
link-level agnosticism no longer depends on constructing the deprecated bags.

`test_synchronous_task_wait` exercises flags 1/2/3 through the shipping completion seam and proves synchronous
completion does not mutate the retired wait counter. The normal `cpp_style` CTest runs the reusable
`tools/check_cpp_style.py`: `clang-format` over first-party non-generated C/C++, the 1,200-line default plus
shrink-only legacy caps, and `clang-tidy` over every first-party C++ translation unit represented in the real
Clang compile database. Vendor and generated sources are excluded. Consumer repos invoke the same implementation
with `--root` and `--compile-commands`; source-free scaffolds must declare that state with
`--allow-empty-scaffold`. `--tidy-touched` is an explicit local fast mode, never the normal CTest.

```bash
cmake -S . -B build          # standalone psxport configure (from a game tree: -S external/psxport)
cmake --build build -j6
ctest --test-dir build --output-on-failure
```

### The agnosticism RATCHET: `game_literals` (`tools/lint/game_literals.py`)

Two entries are the `game_literals` Python gate and its detector selftest. `game_literals` scans `runtime/**` and
`tools/recomp/**` for hardcoded GUEST addresses — one game's fact compiled into the library every
port links — and `game_literals_selftest` gates the detector that certifies it. `psxport_smoke` is
structurally blind to this leak class: a byte-faithful transcription of another game's functions
contains no game SYMBOLS. Spec and burn-down order: `docs/plans/game-seam-redesign.md` §7 / §6.

```bash
python3 tools/lint/game_literals.py            # the gate (also ctest `game_literals`)
python3 tools/lint/game_literals.py --selftest # both classes: must-flag and must-not-flag
python3 tools/lint/game_literals.py --write-baseline   # ONLY to record a deliberate burn-down
```

It is a **ratchet**: `tools/lint/game_literals_baseline.txt` records today's flagged
`<file>:<value>:<count>`, a count may only go DOWN, and going down requires regenerating that
tracked file in the same reviewed change. `--write-baseline` REFUSES to record more than the
baseline already holds without `--grow` (the normal gate never passes it). Only LIVE code is
gated; the same values in comments and strings are counted and listed separately at the bottom of
the baseline, because sizing seam work off a grep count that mixes the two has misled this project
before (`pc_scheduler.h` reads 17 in a raw grep and **0** in live code). Two legal remedies for a
failure: move the fact across the seam (§2 decides which mechanism), or annotate the line
`// psx-console: <the console fact>` when the value really is a console constant the classifier
cannot know. A scan that matches zero files exits **2** saying it scanned NOTHING — never clean.

### The `cfg_*` -> `lucent::` sweep: two instruments, in `tools/`

Retiring the printf-style `cfg_log*` shim is a several-hundred-site mechanical edit, which is exactly
the kind that loses lines silently. Two tools make it checkable:

```bash
tools/syntaxcheck.sh runtime/recomp/foo.cpp     # compile ONE TU with the project's real flags,
                                                # -fsyntax-only, WITHOUT touching build/ — several
                                                # agents share one build dir and a concurrent
                                                # `cmake --build` corrupts it.
tools/logsig.py runtime/recomp/foo.cpp          # one canonical message template per call site
tools/logsig.py --selftest                      # prove the extractor fires, and can say NO
```

`logsig.py` canonicalises the printf spelling and the `std::format` spelling of a call site to the
SAME string (`cfg_logf("cd","%08X")` and `lucent::debug("cd","{:08X}")` both become
`debug|cd|{:08X}`), so before/after over the converted files must diff EMPTY. Its denominator is
every site in the files, not just the ones a run happens to execute. Validate the comparison itself
by deleting one known template and confirming the diff names exactly it — an empty diff from a broken
comparison reads identical to success. It cannot see argument ORDER, and a non-literal format string
is reported as `<non-literal>` rather than dropped. It exits non-zero on a scan that found nothing.

### Function reachability tracer

`runtime/recomp/fntrace.cpp` owns `PSXPORT_FNTRACE=<main-entry>[,...]`. The tracer claims a generated
override slot only after reading its exact current raw owner through the optional generated
`shard_get_override` seam. Its hook temporarily restores and chains that owner, then reinstalls
itself; only a genuinely empty prior slot redispatches to the retained generated body. Installation
order remains semantic: both framework boot spines call `fntrace_init()` after game registration,
and repeat initialization refreshes a site whose title/PlatformHle owner changed while retaining the
prior owner where the tracer is still installed. Old generated substrates remain source-compatible
through the getter's null default, but FNTRACE loudly installs nothing until they are regenerated.
`tests/test_fntrace_init.cpp` exercises registry-owned and raw PlatformHle-shaped owners, repeat
initialization, one-shot parsing/range rejection, and the missing-getter refusal.

`RecompRegistry::substrate_id` separately owns exact generated-code provenance. `emit.py` hashes the
translation units named by the generated source manifest plus MAIN/overlay routing metadata, emits the
full versioned SHA-256 as `g_rec_substrate_id`, and the consumer adapter passes that symbol through the
registry. Installation logs the identity unconditionally; a pre-identity substrate is `UNKNOWN`, never
silently equated with the generated files currently on disk. `test_emit.py` exercises the shipping
emitter with two different bodies and `test_recomp_identity.cpp` covers the runtime install seam.

The initial wiring was also exercised in the Vagrant consumer: one bounded shipping run reported
five requested boot/CD entries as `REACHED` and one valid but later entry as `NEVER CALLED`. Before
this fix the same declared configuration was unread and printed neither class because no boot path
called the initializer. The tracer still deliberately supports MAIN entries only; that limit is
stated in `fntrace.cpp` and is not papered over by a fallback.

**Adding a test = adding ONE file.** `tests/CMakeLists.txt` GLOBs `tests/test_*.c` and
`tests/test_*.cpp` (`CONFIGURE_DEPENDS`, so a plain `cmake --build` re-configures and picks up a new
file), makes one executable + one `ctest` entry per file, and links it against `psxport`. There is
deliberately **no explicit source list**: several agents add tests concurrently and a list would make
every new test a conflicting edit to one shared file. Do not convert it to a list. (If a new file
does not appear, re-run `cmake -S . -B build`.)

Write the test against **`tests/testutil.h`**, the counted assertion harness:

```cpp
#include "testutil.h"
static void test_thing(void) { CHECK(setup()); CHECK_EQ(compute(2), 4); }
int main(void) { RUN(thing); return pt_summary(); }
```

`CHECK` / `CHECK_EQ` (prints both values) / `CHECK_STREQ` / `CHECK_MEM_EQ` (names the first differing
byte) count every check and abort the case on the first failure; `RUN` prints `PASS (n checks)` per
case and a summary line. **A case that asserts nothing is reported as a FAILURE**, and so is a binary
that `RUN`s no cases — `assert()` cannot give you that (it vanishes under `NDEBUG`, and an empty test
exits 0 exactly like a passing one). There is intentionally **no `skip()`**: a suite that reports
"skipped" reads as green while covering nothing.

- `tests/test_harness_selftest.cpp` asserts the harness's own failure paths still fire (a failing
  check counted + short-circuiting the case, an empty case going red, the exit code). A harness
  nobody has seen fail is not a harness — this one re-proves it every run.
- `tests/test_vram_xfer_rect.cpp` pins the GP0 A0/C0 transfer-rectangle decoder shared by CPU→VRAM
  uploads and VRAM→CPU readbacks: coordinate masks, independent width/height fields, and the PSX
  zero-size encoding for a full 1024×512 transfer. It is deliberately a small pure helper test;
  the integration round-trip remains `test_vram_readback.cpp`.
- `tests/test_no_game_address_literals.cpp` is the **game-agnosticism gate**: it scans `runtime/` +
  `common/` for hex literals in LIVE CODE (comments and string literals are RE documentation and do
  not count) that name a particular game's guest memory — main RAM `0x80010000-0x801FFFFF` and its
  KSEG1 mirror, plus any scratchpad FIELD `0x1F800001-0x1F8003FF` — while every console constant
  (`0x1F801xxx` HW regs, the BIOS/kernel region, `0x200000`, segment masks, the PS-EXE load base and
  initial SP) is asserted NOT to trip it. The repo holds **391** such literals today, recorded as a
  per-`(file, address, count)` **baseline in the test itself**; the gate fails on anything NEW, and
  also fails when a count is too HIGH (someone fixed a literal without shrinking the baseline) or a
  row matches nothing (delete it). So the baseline is shrink-only, and raising a count is a
  hand-written edit to the gate, visible in review. The count is printed every run. Fix one by moving
  the address into `GameConfig` (`runtime/recomp/game_iface.h`) — reuse an existing field — and obey
  the honest-zero rule: a consumer reading `0` must fail fast or announce its blindness once, loudly
  (`runtime/recomp/ot_attr.cpp`'s `pool_range()` is the worked example).
- `tests/test_config_enh.cpp` is the **anti-divergence gate on the enhancement suppression rule**. A
  pc_enh knob must resolve to OFF under `PSXPORT_ORACLE` or either SBS form whatever the CVar ladder
  said, and there must be exactly ONE definition of what a byte-compare run is: while `PSXPORT_ENH` was
  env-only, a consuming game had to keep a second copy of the rule, and if two copies diverge one fails
  to recognise an SBS variant while the contaminated compare still looks clean. The gate compares
  `psx::config::compare_run_from()` against the hand-written three-input expression over all 8 input
  combinations, asserts the suppression notice fires ONCE PER KNOB and NAMES both the knob and the input
  that tripped it, and checks the migrated parse against the pre-migration body over 36 (value, name)
  pairs. Two cases exist because sabotage showed the rest of the file could not see the defect: one
  moves `PSXPORT_ORACLE` mid-process through `set_runtime()` with **no caches cleared** (every other case
  resets between configurations, so none of them notices a gate that answers from a one-time binding),
  and one is a TRIP-WIRE that fails when `PSXPORT_SBS`/`PSXPORT_SBS_MODE` become CVars while
  `compare_run()` still reads the environment. `psx::config::selftest()` drives `enh_gate()` — the
  shipping function, not the pure predicate beside it — over both classes in one process.
- Tests link the `psxport` static archive, so only the objects a test references are pulled in. A
  unit that calls into the game substrate fails to LINK on the `generated/` symbols
  (`main_dispatch`, `g_rec_overlays`, `rec_func_index`) — that is the framework/game seam saying the
  unit is not testable in isolation, not a harness bug.
- The suite is wired from the **root** `CMakeLists.txt` only. A consuming game includes
  `cmake/psxport.cmake` directly and never processes it, so the gate is run from a standalone
  psxport configure (which works fine from inside a game tree's `external/psxport`).
- `tools/fmv_export/test_fmv_decode.cpp` is a separate, older suite with its own build script
  (`tools/fmv_export/build.sh`) — it is disc-gated in part and is not in `ctest`.

## Verifying a change

There is no independent console/emulator lockstep oracle. SBS can compare the native/recompiled A leg
against a pure interpreter B leg, which is useful for game-state and packet-generation differences but
still shares psxport's hardware models. The Beetle GPU tee independently checks rasterization given one
command stream. Neither instrument may be cited beyond that declared scope. To verify:
- **Engine / render work:** run the game (`./run.sh`, or `PSXPORT_VK_HEADLESS=1` + REPL `shot` for a
  headless screenshot) and observe it. The USER verifies visually; the agent builds, sends pics, and
  inspects state via the REPL / debug server.
- **Content-interface correctness** (guest RAM the PSX AI/physics read): inspect that RAM via the REPL
  (`r`/`rw`/`dumpram`, with the `.spad` scratchpad sidecar) and reason about correctness from the recomp
  reference + the live game — there is no automated RAM-diff gate anymore.
- **Principle:** don't conclude from a cherry-picked still; verify on the running game.
- **Headless behavioral self-tests** (`PSXPORT_SELFTEST=<name>`, runtime/recomp/selftest.cpp): deterministic
  RED/GREEN assertions on a single full-PSX (psx_fallback) core — no render, no oracle. Exit 0=pass, 1=fail
  (so CI/run.sh can gate). `=startgame`: mash Start, assert the field reaches free-roam (sm[0x48]==2) and the
  GAME loop runs (guards the recompiler stage-split freeze, later-269) AND the intro XA clip plays→completes
  headless (later-270: the SPU/XA stream is now advanced even without an audio device, so audio-gated game
  logic progresses headless). `PSXPORT_SELFTEST_VERBOSE=1` traces per-frame stage/SM. Add a new case in
  `selftest_run()` — and note the CONSUMER must call `selftest_run()` from its `main()`, or none of these
  can run at all (Spyro's did not, so the whole suite was unreachable from that port until 2026-08-04).
  `=spuirq`: drives the SPU register file (IRQ address, transfer address, SPUCNT bit 6) and asserts the
  SPU's interrupt line reaches `Hle::i_stat` bit 9 edge-latched, with a negative control. It exists
  because NO game run exercises that path — Spyro sets SPUCNT bit 6 in 0 of its 172 SPUCNT writes. (The full-PSX field still doesn't progress PAST the intro cutscene under coroutines — a
  known diagnostic-path limit, docs/findings/sbs.md; the shipping NATIVE field is unaffected.)

## Rendering, present, and config — see the dedicated docs
- **`docs/render-arch.md`** — the GP0→screen path, the VK rasterizer + present dispatch
  (`blit_src`→`gpu_vk_present`), headless **offscreen VK** (`PSXPORT_VK_HEADLESS`), and the depth model.
  Render is ONE PC-native behavior: native per-pixel depth is ALWAYS on (no faithful/OT-order toggle),
  single VK panel, the render queue owns ordering. Read before touching graphics/VK.
- **`docs/config.md`** — the `cfg` module (`cfg_on/cfg_int/cfg_str/cfg_dbg`). Diagnostics are REPL-driven:
  set the unified `PSXPORT_DEBUG` channel set at runtime via the REPL / debug-server `debug <chans>`
  command (`cfg_dbg_set`). Don't add raw `getenv("PSXPORT_…")`.
- **`docs/driving-the-game.md`** — how to DRIVE the port to a scene via the REPL (`PSXPORT_REPL=1`,
  `newgame`/`skip`/`press`…), the live debug server (`tools/dbgclient.py`), and scene-state signals.
  Read before driving the game.

## Where state/notes live
- `docs/journal.md` — chronological findings + dead ends (read the head before re-deriving).
- `<local-notes>/.../memory/MEMORY.md` — cross-session pointers (machine-local; in-repo docs are canonical).
- `scratch/` — gitignored artifacts by type (`bin/ wav/ screenshots/ raw/ logs/ state/`). Never `/tmp`.

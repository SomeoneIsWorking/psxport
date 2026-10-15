# PSX workspace restructure: one pass

Measured on 2026-10-14: psxport `93e0f1ab` with an uncommitted M8a tree, and every title repo at a clean HEAD. Line numbers drift, so re-read a file before you edit it. "Delete" means the code is gone. Nothing is kept as a shim, forwarder, alias or second copy once the pass ends.

## 1. Rules

- **Inheritance, not configuration.** psxport holds only what every title shares, and it calls the title through virtual methods. Game-specific behaviour is a title subclass of a small psxport base. Guest addresses, hooks, DMA tables and title logic stay in the title's own code. The framework never takes a descriptor struct, a facts bag, an address table or a callback table and interprets it. Small identity data stays plain data: disc serial, size, hash, EXE header, card names and menu rows.
- **Each piece of code is touched once.** psxport gains its final title interface, then every title moves to it, then psxport deletes the old surface. The old surface lives only until (c); no title uses it after its own pass.
- **One agent at a time.** Each repo gets one brief. A repo is committed and pushed as soon as its brief lands (section 5 (d)).
- **No baselines.** No run of the current port is a reference. Checks are absolute (section 6).
- **Paths name the concept, never the title.** The repo, or `titles/<t>/` in a multi-title repo, already names the game. `crashbash/game/runtime/runtime.cpp`, not `crashbash_runtime.cpp`; `spyro/titles/spyro1/render/sky.cpp`, not `spyro1_sky.cpp`. The namespace carries the title (`crashbash::Runtime`, `spyro1::render::Sky`). A file is included through its directory (`"render/sky.h"`). Renames go through `git mv` in the repo's pass; repos already passed get one rename sweep before (c).

## 2. Where the tree is

- **Done (M1-M7).** `psx::host::ProductHost::run` is the only process top level and `product_main.cpp` is the only `main`. Every title runs through `TitleSession` with a `FrameDriver`. `native_boot`, `boot/machine.*`, `ts2::TitleSession`, `c12::Machine` and the title mains are gone. Tomba 1 is in `TombaCatalog`, and Spider-Man 2 is in the Spider-Man catalog.
- **Uncommitted M8a (psxport, about 120 files).**
  - `Core(GameRuntime&)` and `Game(GameRuntime&[, GpuDevice&])` exist.
  - `psxport_install_game`, `psxport_game_runtime`, `installedRuntime` and `g_ownedLegacyRuntime` are deleted.
  - `tests/support/stub_game_runtime.{h,cpp}` and the `psxport_test_support` target are new, and 82 test files are edited.
- **Consequence.** No title builds against the live `external/psxport` today, because about 120 references in 69 title files still call the deleted install functions. Until a title's own pass lands, it can only be run from its existing `build/` binary.

| repo | render path today | interp | GTE call sites | accepted.txt (global fn / block static / extern) | codemap lines |
|---|---|---|---|---|---|
| psxport | - | - | 260 | 1669 / 203 / 33 | 270 |
| spyro | S1 Native, S2 Record, S3 Gte | S1, S2, S3 | 579 | 45 / 12 / 0 | 414 |
| toystory2 | Record | yes | 40 | 36 / 9 / 0 | 189 |
| crashbash | Record | yes | 21 | 0 / 2 / 0 (+1 entry point) | 262 |
| crash | C1/C2 Record, C3 Gte | C1/C2 (no producers) | 49 | 6 / 2 / 0 | 166 |
| ctr | Record | no | 3 | 0 | 153 |
| Tomba2Engine | T2 Record, T1 Gte | T2 | 413 | 374 / 65 / 3 | 1263 |
| spider1 | SM1, SM2 Gte | no | 0 | 8 / 0 / 0 | 223 |
| vagrant | Record | no | 6 | 1 / 1 / 0 | 327 |
| megamanx4 | Record | no (native 60) | 4 | 1 / 0 / 0 | 270 |
| tekken3 | Record | no (native 60) | 0 | 0 | 239 |
| c12 | Record | no | 86 | 0 | 95 |

No repo has a `docs/features.md`. psxport and nine title repos use the narrowed `.clang-tidy` (an explicit list under `-*`). vagrant and c12 already enable `clang-analyzer-*`, `bugprone-*` and `performance-*`.

## 3. End state: psxport

### 3.1 Title interface (`namespace psx::game`)

| type | role |
|---|---|
| `GameRuntime` | One per title, process lifetime, immutable. The only title interface. It has no legacy views, no `Core::cfg`/`hooks`/`gameCtx`, and no `if (core.cfg)` anywhere. |
| `TitleSessionState` | Per-Core RAII base returned by `createSession(Game&)`. The title derives its own. It replaces `createContext`/`destroyContext`/`void *gameCtx` (91 references in psxport, 16 title overrides). |
| `FrameDriver` | Required. `stepFrame` is one guest turn plus exactly one `presentation.commit`. |
| `TitleAudio`, `TitleDevWarp` | Optional capabilities returned by `audio(Core&)` and `devWarp(Core&)`. The null check lives only in the consumer (`SpuAudio`, the menus, `warp_control`). |
| `HostIdentity`, `TitleIntSetting`; `psx::host::TitleIdentity` (`host/title_catalog.h`) | Plain data: disc serial, size, sha256, EXE header, disc env key and path; card and user-data names; menu rows. |

`GameRuntime` virtuals, final list:

- **Boot.**
  - `admitExecutable`.
  - `startProgram(Core&)`. Its default does nothing, so the frame driver enters at the EXE header entry `admitExecutable` set. A title that needs crt0 replay calls `psx::game::Crt0Boot` (`game/crt0_boot.h`) with its own `Crt0Image`.
  - `registerOverrides(Game&)`.
  - `createSession`, `createFrameDriver`. A title's task scheduler is its own session member, driven from `stepFrame`.
- **Hardware services the title owns.** `deliverPad`, `stockCdReadLanded`, `guestCodeModuleWindow`, `packetPool(Core&)` (the pool's current extent; the census re-reads it once per logic frame and after a store to one of its bounds). The host-pump per-sector ready call is `StockLibcd::callReady`, a virtual on the title's libcd; a title whose guest INT1 service delivers it binds `GuestInterruptLibcd`.
- **Presentation.**
  - `interpolates()`.
  - `stateProducer(Core&, uint32_t producer)`.
  - `sealedFrameIsCut`, `pacePresentation`, `displayHeight`, `guestWidescreenProjection`, `panelLogo`.
- **Debug and run.** `titleCommand(Core&, cmd, line, FILE *reply)`, `inputPhase`, `nativeState`, `reportRun`.

### 3.2 What titles hand psxport today, and what each becomes

| today | read by (psxport) | end state |
|---|---|---|
| `GameConfig` (about 70 fields) + `GameHooks` (25 fn pointers), bound by `LegacyGameRuntimeAdapter` (Tomba 2) and `bindLegacyInterface` (X4) | 25 files, every `if (core.cfg)` | Deleted. Each field goes to a row below or into title code. |
| `GuestProgramImage`: bss, stack, heap, gp, libcInit, gameMain, crt0Entry, residentText, backtraceText, stackBias, `BootEntry` | core, crt0_boot, title_session | `startProgram` virtual. The crt0 addresses become the `Crt0Image` the title passes to `Crt0Boot`. No `residentText()`: no framework code reads `residentText`/`backtraceText`. |
| `PlatformHlePlan`: 16 bindings, the libgte/libcd/libgpu/VSync/GPU-timeout addresses, `dmaCallbackTable`, `stockCdWorkArea`, HLE windows | platform_hle, hle_interrupt, cd_override, stock_cd_work_area | Deleted. psxport exports the stock-library implementations as classes holding their bodies (`psx::cd::StockLibcd`/`GuestInterruptLibcd` in `cd/`, `StockLibgpu`, `StockLibgte`, `StockVSync`, `StockDmaCallbacks` in `hle/`). The title owns the objects, keeps them alive while its Game runs, and binds them at its own addresses in `registerOverrides`. |
| `GuestPadBufferLayout` (slot buffers, pointer table, stride) | pad_input | `deliverPad` virtual. The default is the BIOS pad path. A buffer-writing title calls `psx::input::StockPadBuffers`. |
| `GuestCdStreamCallbackLayout` (ready callback pointer, delivery owner, status) | cd_ready_delivery, cd_override, cd_stock_read_completion | The ready slot is a `StockLibcd` constructor argument; the host pump is the base class (`callReady` virtual), the guest-INT1 owner the `GuestInterruptLibcd` subclass with its `Completion`. |
| `GuestPacketPoolWindows` (three representations) | ot_attr, guest_packet_pool_windows | `packetPool(Core&)` virtual returning a `PacketPoolExtent`; the layouts move to `game/guest_packet_pool_windows.h` for a title to answer with. |
| `RenderCapabilities` {defaultPath, nativeRenderPath, temporalInterpolation, defaultFaceOrder} + `render_path_install` | game.cpp, render_path | `interpolates()` virtual. The path is always Record. Both the struct and the install are deleted. |
| `guestVramIsPicture`, `guestPictureIsNativeWidth`, `createTemporalFramePresentation` | renderer | Deleted with Native. Keep `guestPictureIsNativeWidth` only if Spyro 1's boot logos on Record need it. |
| `core.stateProducers.install(addr, …)` registry | frame_composer | `stateProducer(Core&, addr)` virtual, answered from the title's session state. |
| `replCommand`, `controlCommand`, framework `tp` (`dbg_server.cpp:653`, `repl.cpp:311`) | repl, dbg_server | `titleCommand`. `tp` becomes a Tomba 2 command. |
| hooks `audioMixFrame`, `audioNowPlayingName`, `audioSoundTestPlay` | spu_audio, menus | `TitleAudio`. |
| hooks `devAreaCount`, `devAreaName`, `devWarpAllowed` | warp_control | `TitleDevWarp`. |
| hooks `sched*`, `hasNativeHandlerForEntry`, config `schedEntries`/`syncWait*`/task table; `frame/pc_scheduler.*` (554 lines), the Tomba stanzas of `platform/scheduler.cpp`, `platform/synchronous_task_wait*`, `task_slot_layout.h` | frame, platform, gpu_perf | Tomba 2's `TombaScheduler`, a `TombaSession` member. |
| config `cdFileLoad`, `cdAsyncRead`, `cdInlineLoad`, `cdCmdStream`, `voicePlay`/`voiceStop`, `lastSectorTracker`, `cdCallbackTable`/`Fn`; hooks `cdDialogToneActive`, `cdMusicFadeIn` | cd_override (1,049 lines) | Tomba 2 overrides. `cd_override.cpp` keeps only stock libcd. |
| config `paceQuota`, `guestDisplayHeight`, `preserveVramBackdrop`, `vsyncTrap`, `fadewatch*`, `codeScan*`, `decDct*`, `gpuTimeout*`; hooks `renderFadeState`, `renderBbFrameReset`, `frameUpdate`, `drawOTag`, `musicCoordTick`, `replCamTeleport*` | frame_pacer, gpu_native_scanout, gpu_vk_fadewatch, hle | `pacePresentation`/`displayHeight` virtuals, the stock HLE classes, or title code. Hooks with no live caller are deleted. |
| `GuestWidescreenProjection` | presenter | Already a title subclass. `guestCoordinatesWidened` is deleted with Gte. |
| override registration (`Producer{Arg}` tag, native dispatch) | native_dispatch | Stays. The title registers its own natives. |

### 3.3 Renderer

- Beetle `gpu.c` is the only VRAM and the only GP0 executor.
- `RenderPath = {Record, Device}`. Device is the oracle and diagnostic view.
- `FrameComposer` builds every in-between from `StateProducer` renders of saved state, with the shown record as the base. No packet lerp and no keyed blend. `DisplayPassGuard` aborts a guest-memory write inside `StateProducer::render`.
- recordcheck covers t=1 only. In-betweens are covered by per-producer unit tests through `composeFrame`.
- SDL3 GPU stays. `gpu_vk.cpp` (3,592 lines) splits into `gpu_vk_present.cpp` and `gpu_vk_submit.cpp`, each under 1,200 lines.
- **Deleted.**
  - Native: `gpu_native*` (the second VRAM), `render_queue*`, `gpu_painter`, `painter_object_layer`, `face_contest`, `proj_prim`, `ot_lifo_depth`, `guest_packet_filter`, `render_noise`, `native_composite_capture`, `fade_interpolation`, `fps60*`, `host_world_pass_strategy`, `game_hooks_opt`, `gpu_vk_depth`, `gpu_vk_semi_order`, the PC-rasterizer selftests.
  - Gte: `guest_geometry_interpolation`, `guest_geometry_scene_source`, `projection_provenance`, `in_between_splice`, `in_between_strategy.h`, `wide_2d_layout`, the render_path_control player cycle.
  - `keyed_blend.*`.
  - Mods `face_order` and `debug_*`.
  - Env knobs RQ, ZFIGHT, ZBIAS, PAINTER, PAINTFG, PRIMDUMP, POLYDUMP, TEXWATCH, CLUTWATCH, GP0RAW, FADEDBG, VRAMDUMP, CLOBBERDUMP.
  - The tests of all of the above.

### 3.4 Machine, logging, config

- **GTE.**
  - `psx::cpu::Gte` is a `Core` member wrapping `GteRegs`, and titles call `core.gte().writeData(...)`.
  - `gte_write_data`/`gte_read_*`/`gte_op`/`GTE_*` are deleted, and `GTE_CurState` is no longer a process global.
- **GPU.**
  - `GpuDevice&` is an explicit member. `GpuDevice::sInstance`, `gdev()` and the `s_*` macros are deleted.
  - Dumps go into `PrimitiveDump`, and selftests into `psx::gpu::selftest`.
- **Devices.**
  - Each family becomes a class: `DmaRegisters`/`DmaCallbackRegistry`, `Disc`, `XaStream`, `CdController`, `Cd`, `ProjParams`, `SpuDevice`, `MdecDevice`, and `Watchdog` (owned by ProductHost).
  - `guest_mult`/`cpu_div` move to `psx::cpu`.
  - Beetle link-seam globals stay confined to their one adapter file each.
- **Debug.** `DbgServer`, memcensus, store_observe, snapshot, the GpuPerf counters, the render-path live switch and RmlOverlay keep their state as members.
- **Logging.** Lucent only. `cfg_log*`/`cfg_line_*` are deleted: 19 sites in psxport, 123 in Tomba, 14 in X4.
- **Config.**
  - One immutable typed `psx::config` object, built by the config owner and reached through `Game`.
  - `cfg_on`/`cfg_int`/`cfg_str`/`cfg_dbg` (100 sites in psxport, 39 in Tomba, 5 in Spyro) and the 29 CVar externs become its fields.
  - `common/env.cpp` is the only `getenv`. Not yet: `psx::config::Config`, the CVar registry and lucent read the process environment through `lucent::config` (lucent's own `getenv`), `common/env.cpp` serves the disc path, and `cd/disc.cpp` and `platform/memcard.cpp` each parse `./.env` again. (c) folds those into the config owner.
  - Done in (a): `psx::config::Config` (`config/typed_config.h`) with `unpaced` (`PSXPORT_NOPACE`), `repl` (`PSXPORT_REPL`) and `fps60` (`PSXPORT_FPS60`, a launch choice over the saved setting), built once by `ProductHost` and held as `game.config`. The live 60 fps setting is `Mods::fps60`. Title-declared `pc_enh` CVars (C-12, X4) stay CVars until a title-settings owner replaces them: `TitleIntSetting`, which stays data, binds an `IntVar`.
  - Gap: no typed title launch config. Tomba's launch knobs (`PSXPORT_NO_FMV`, `PSXPORT_AUTO_SKIP`) stay file-scope CVars read once into `TombaSettings`.
  - `OtAttr::trackStore` reads its field as a plain member load.
- **C linkage.** `extern "C"` remains only on functions Beetle's C calls back.

### 3.5 Include split

- Titles see only the title API: `game/` (`game.h`, `game_runtime.h` with `FrameDriver`, `crt0_boot.h`, `psx_exe_image.h`, `title_settings.h`, `guest_packet_pool_windows.h`), `host/title_catalog.h`, `host/single_title_catalog.h`, `present/state_producer.h`, `present/frame_composer.h` (`PrimitiveSink`), `gpu/guest_widescreen_projection.h` (not yet under `present/`), `present/gte_control.h` (`GteGuard`), `core.h` guest memory, `cpu/gte.h`, `cpu/gte_registers.h`, `frame/logic_frame.h`, `input/pad_delivery.h`, `input/input_phase.h`, `host/logo_image.h`, `core/guest_address_range.h`, `core/guest_code_module.h`, the guest-call seam in `runtime/cpu/` (`execution_exit.h`, `execution_control.h`, `guest_call.h`, `native_dispatch.h`, `image_identity.h`, `lightrec_executor.h` for the frame driver's turn), `config/config_var.h`, `config/enhancement_gate.h` and `config/stepped_range.h` (title `pc_enh` knobs), and the stock classes (`cd/stock_libcd.h`, `hle/stock_*.h`, `input/stock_pad_buffers.h`). C-12's pass needed every header added here; Tomba's added `platform/coro.h` (the continuation a title's scheduler runs its tasks on), `audio/xa_state.h` (the XA streamer a title's own CD chokepoints drive), `present/gp0_primitive_decode.h`, `present/state_bytes.h` and `present/host_ordering_table.h` (producer decode and emission), `hle/platform_hle.h` (`bindStock` for title-owned library bodies), `cpu/execution_services.h` (`handleBreak` for a native mirror of a guest `break`), `state/machine_state.h` and `state/state_blob.h` (a title's `nativeState` port), `game/disc_executable.h` (a boot stub's hand-off to the disc executable), `core/guest_abi.h` (the guest stack frame a native body mirrors), `present/emit_memory.h` and `present/blend.h` (a producer's guest memory and its blends), and `cpu/resumable_guest_call.h`. Spyro's pass adds `present/frame_state.h` (`stateAs` for a producer's saved state) and still includes, with no title-facing replacement yet: `frame/frame_pacer.h`, `hle/hle.h` (VBlank IRQ poll), `runtime/cpu/host_turn.h` and `core/snapshot.h` (Spyro 1's field owner paces fields, polls VBlank, registers its host turn and ticks the snapshot), `cd/disc.h` (WAD sector reads), `gpu/ordering_table.h` (the flatten names and assigns `core.otTables` depth bins), `present/frame_record.h` (`OtSlot` for the field producers), and `core.gpuDevice.vram()` through `core.h` (the picker logo reads the device's VRAM); its tests add `config/typed_config.h`, `frame/pace_plan.h`, `frame/emulated_time.h`, `host/title_selection.h`, `cd/cdc_state.h`, `cd/cd_stock_read_completion.h`, `crt0_fixture.h` and `mips_asm.h`. It includes none of the native renderer's headers. Tests may also include `testutil.h` (`psxport_test_support`) for checks, `frame/frame_loop_shell.h` to step a `FrameDriver` through the product loop, `stub_game_runtime.h` (`psxport_test_support`) for a Game with no title, and `boot/hw_bind.h` (`psxport_internal`) for `gte_init` in a GTE-driven test.
- Leaks to close before `gpu/` and `cpu/` go private: `core.h` includes `gpu/gpu_device.h`; `game_runtime.h` includes `gpu/render_capabilities.h` (gone with `RenderCapabilities` in (c)); `game/psx_exe_image.h` includes `runtime/cpu/image_identity.h`; the packet-pool layouts live in `gpu/guest_packet_pool_windows.h` (moved to `game/` in (a)). Tomba's `BootCards` presents the SCEA card through `gpu/gpu_vk.h` `gpu_vk_present_image` and `scea_asset.h`, for want of a title-facing host-picture present. Core also holds `GpuDevice`, `OtTables` and `RenderSubstrate` (gpu/, debug/) by value, so `gpu/` cannot go private until (c) shrinks RenderSubstrate or Core holds them behind a pointer.
- `boot/`, the `host/` internals, `gpu/` and the `cpu/` internals are PRIVATE in `psxport.cmake`.
- Title includes today: core 15-375 per repo, gpu up to 124 (Spyro), present up to 33 (Crash Bash), config up to 97 (Tomba). Each title's pass removes its own.

### 3.6 Docs and policy

- **psxport `docs/features.md`.** Sections F-BOOT, F-TURN, F-INPUT, F-PICK, F-RENDER, F-WIDE, F-FPS60, F-FMV, F-CD, F-AUDIO, F-LOAD, F-SAVE and F-DEBUG, each at most 40 lines. Hop rows are written as `` `path:Symbol` ``.
- **Every title's `docs/features.md`.** Title hops only, joined as `psxport F-X.n`. C-12 (the template) joins F-BOOT.1 (catalog and disc), F-BOOT.6 (`registerOverrides`, `createSession`), F-TURN.2 (`FrameDriver::stepFrame`), F-TURN.4 (run-end report), F-CD.1 (stock libcd binding), F-CD.3 (module landing), F-RENDER.1 (render path), F-RENDER.2 (`packetPool`), F-WIDE.1 (projection policy) and F-WIDE.2 (widened culls). Tomba adds F-BOOT.5 (`startProgram`), F-TURN.3 (the title's task scheduler, stepped by its frame driver), F-INPUT.1 (`deliverPad`), F-RENDER.3 (`displayHeight`), F-FPS60.1 (`stateProducer`), F-FPS60.2 (`sealedFrameIsCut`), F-FMV.1 (a title-started movie), F-AUDIO.1 (`TitleAudio`), F-SAVE.1 (`hostIdentity`), F-SAVE.2 (`nativeState`), F-DEBUG.1 (`titleCommand`), F-DEBUG.2 (`TitleDevWarp`) and F-DEBUG.3 (`worldReadout`). Spyro adds F-TURN.5 (`pacePresentation`), F-INPUT.2 (`inputPhase`), F-PICK.1 (`panelLogo`) and F-RENDER.4 (`guestPictureIsNativeWidth`). psxport's `features.md` keeps those hop numbers.
- **Codemap.** At most 150 lines in Owners-table shape, with no source-tree dump and no "Who owns it" chains.
- **Renderer doc.** `presentation.md` is the one renderer doc. Delete `one-renderer.md` and `presentation-contract.md`.
- **cpp_policy** (re-harness `cpp_policy.py`, called by psxport `check_cpp_style.py`):
  - `entry point` with no exceptions.
  - `global-namespace function`, `extern declaration` and `block-scope static` with every `accepted.txt` empty.
  - `retired name` gains `psxport_install_game`, `bindLegacyInterface`, `cfg_logf` and `gte_write_data` for title repos.
- **Size caps.** `PSXPORT_CAPS` loses the `gpu_native.cpp`, `render_queue.cpp`, `gpu_vk.cpp`, `game_iface.h`, `render_path_control.*` and `test_synchronous_task_wait.cpp` rows.
- **clang-tidy.** Every repo's `.clang-tidy` is `clang-analyzer-*`, `bugprone-*` and `performance-*`, with warnings as errors.
- **Tooling copies.** One `re_frontier.py`, re-harness's. Delete the copies in psxport `tools/port/`, spyro, spider1, megamanx4 and toystory2. Any `tools/source_policy.py` that duplicates `tools/policy/harness.py` is deleted (spyro, spider1, crashbash, c12).

## 4. End state per title repo

Each pass converts the rows of section 3.2 that the repo uses, moves GTE calls to `core.gte()`, moves logging to Lucent, moves config reads to typed config, empties the repo's `accepted.txt`, aligns `.clang-tidy`, writes `features.md` and a slim codemap, and adds a gameplay route where the end play check has none. The tables below list only what is specific to each repo.

**Size key:** S is a mechanical migration of 1-2 sessions; M adds title-owned seams; L adds RE or product render work; XL is L across several titles.

### spyro (XL)

| | |
|---|---|
| owners | `Spyro{1,2,3}Runtime : SpyroRuntime : GameRuntime`; `spyro::Context` becomes `SpyroSession : TitleSessionState`; the shared `game/render/terrain` `TerrainStateProducer` |
| converts | programImage, HLE plan, pad layout, cd stream, packet pool (S2), `pacePresentation`, `panelLogo`, `inputPhase`, `guestPictureIsNativeWidth` |
| product: Spyro 1 to Record | 1. Ghidra-read the display tail of `0x8001ED5C` (GamestateDraw in `external/open-spyro`) and split it into the title tail, so the guest builds its OT and calls DrawOTag on the device. 2. Widen the guest culls and anchor the HUD on Record, as Spyro 2 does. 3. Move the native families (terrain, paired actors and mobys, HUD and 2D, sky, scene recipes) to `StateProducer` renders through `PrimitiveSink::emit`, terrain first and on the shared terrain producer where the drawer matches. 4. Set `interpolates()=true`. |
| product: Spyro 3 to Record | Register drawer `0x80022378` with the shared `TerrainStateProducer`, using its own facts. |
| deletes | `game/render/temporal/` (2,529 lines), `TerrainWorldPass`, `terrain_packet_sink`, both `createTemporalFramePresentation` overrides, every RenderQueue use (about 78 files under `game/render`), FrameRenderer's native/reference split, `painter_submission_preflight`, `scene_painter_order`, `SpyroRuntime`'s `widescreenOnly` default, `tools/re_frontier.py`, `tools/source_policy.py` |
| size | GTE 579 sites; accepted 45 + 12; codemap 414 to 150 or fewer; the 20 docs that restate the 60 fps mechanism link to F-FPS60 |

### toystory2 (L)

| | |
|---|---|
| owners | `ToyStory2Runtime`; producers in `game/render/{actor,slot_mesh}_producers.cpp` |
| converts | programImage, HLE plan, pad, cd stream, packet pool, `inputPhase`; `hostIdentity` stays data |
| product | Keyed-only objects get state renders: the negative-header static meshes (issue 0041, `0x80012FE0`) and the remaining part drawers including DCPL `0x8001CD34` (issue 0040). The census must show 0 keyed-without-render primitives. |
| deletes | `tools/re_frontier.py` |
| size | GTE 40; accepted 36 + 9; install-call references 10 in 5 files |

### crashbash (S)

| | |
|---|---|
| owners | the `title_adapter.cpp` runtime; `game/render/state_renders.cpp` producers |
| converts | programImage, HLE plan, packet pool, `inputPhase`, `nativeState`, `admitExecutable` |
| product | `packaging/linux/launcher_main.cpp` and the `getenv` in `user_paths.cpp` fold into ProductHost provisioning through the shared first-run setup UI. That clears the last `entry point` exception. |
| deletes | the launcher `main`, `tools/source_policy.py` |
| size | GTE 21; accepted 2 static + 1 entry point; install references 11 in 10 files |

### crash (M)

| | |
|---|---|
| owners | `Crash1Runtime`/`Crash2Runtime` (Record), `BoundaryRuntime` (Crash 3) |
| converts | programImage, HLE plan, pad, `stockCdReadLanded`, `sealedFrameIsCut` |
| product | Crash 3 moves from Gte to Record, with a 16:9 canvas showing the margins. Crash 1/2 interpolation is unchanged: there are no producers, so S007 stays missing (feature work, outside this pass). |
| size | GTE 49; accepted 6 + 2; install references 17 in 10 files; warp exists |

### ctr (S)

| | |
|---|---|
| owners | `CtrRuntime` (Record, no interp) |
| converts | programImage, HLE plan, cd stream, `sealedFrameIsCut` |
| size | GTE 3; accepted 0; install references 20 in 11 files; replay `arcade-crash-cove-drive.pad` |

### Tomba2Engine (XL)

| | |
|---|---|
| owners | `TombaRuntime : GameRuntime` directly, no adapter; `TombaSession` from `game_ctx`; `TombaScheduler` held by `TombaSession`; `TombaAudio : TitleAudio` (per-Core NativeMusic); `TombaDevWarp : TitleDevWarp` (`dev_areas.cpp`); `Tomba1Runtime` |
| converts | every row of section 3.2 |
| product: scheduler | Moves into `game/core/sched/`, and the code is cut from psxport in this brief. The 12 hook sites become direct `Engine` calls. `test_synchronous_task_wait` and `test_task_slot_layout` move here. Read Tomba 1's `GuestTaskBudget` first, so no second scheduler is made. |
| product: CD | The chokepoints become Tomba overrides, cut from psxport in this brief. cdCommand/cdSync/cdReadPrim/cdInit use the stock classes only where Ghidra shows a stock body. |
| product: keys | Issue 0033: resolve the ambiguous writers `0x8007E6DC`, `0x8003C2D4`, `0x8002AB5C`, `0x80106824`, `0x8007E1B8` and `0x8007E9C8` into producers. |
| product: Tomba 1 | Moves from Gte to Record so psxport keeps one renderer. Native 60 fps, so `interpolates()` is false and it needs no producers; the work is a widened canvas plus widened guest culls. |
| product: other | `tp` becomes a Tomba command; delete `extern tomba_repl_command`; `asset.cpp:316 gpu_native_load_image` becomes a device upload; `render_noise.h` and `gpu_vk_fadewatch.cpp` lose their Tomba addresses |
| deletes | `game/core/entry/{game_hooks.cpp, game_config.cpp, title_facts.h}`, the `getenv` in `graphics_bind.cpp` and two `ai/beh_*` files, the 61 function-local and 7 mutable file-scope statics |
| docs | The codemap's Guest address index moves to `docs/guest-addresses.md`. `titles/tomba1`'s per-directory READMEs become codemap rows. Codemap 1,263 lines to 150 or fewer. |
| size | GTE 413; logging 123; config 39; accepted 374 + 65 + 3 |

### spider1 (M)

| | |
|---|---|
| owners | `Spider1Runtime`, `EnterElectroRuntime`, `Spider1Widescreen` |
| converts | programImage, HLE plan, pad, cd stream, `stockCdReadLanded` |
| product | Spider-Man 1 and Spider-Man 2 move from Gte to Record. Delete `Spider1Widescreen::guestCoordinatesWidened`. Each title gets a 16:9 margin shot. |
| deletes | `tools/re_frontier.py`, `tools/source_policy.py` |
| size | GTE 0; accepted 8 (test functions); install references 16 in 3 files |

### vagrant (S)

| | |
|---|---|
| owners | `VagrantRuntime` (Record, no interp); the title render owners |
| converts | programImage, HLE plan, pad, `admitExecutable` |
| product | The 4 `activeRq()` sites (`game/render/{title_startup,title_movie,battle_frame,title_menu}.cpp`) become `presentation.commit`. Widen the canvas, never the horizontal projection word. Add a gameplay route. |
| size | GTE 6; accepted 1 + 1 |

### megamanx4 (M)

| | |
|---|---|
| owners | `X4Runtime : GameRuntime` with no legacy views; x4 boot facts in title code |
| converts | programImage; the HLE windows, vsync trap and DMA callback table become `StockVSync` and `StockDmaCallbacks(x4::stream_startup::kDmaCallbackTable)` bound in `registerOverrides`; pad; card names become `HostIdentity` |
| product | Delete `bindLegacyInterface` (`x4_runtime.cpp:45`) and `game/title/{game_hooks.cpp, game_config.cpp, legacy_game_interface.h}`. The DMA-3 STR callback must still fire. Add a gameplay route. Co-op and dialogue skip stay outside this pass. |
| deletes | `tools/re_frontier.py` |
| size | logging 14; GTE 4; accepted 1 |

### tekken3 (S)

| | |
|---|---|
| owners | `tekken3::Runtime` (Record, native 60) |
| converts | programImage, HLE plan, pad |
| product | Add a gameplay route. |
| size | GTE 0; accepted 0 |

### c12 (S)

| | |
|---|---|
| owners | `C12Runtime`, `C12FrameDriver` |
| converts | programImage, HLE plan, cd stream, packet pool, `guestCodeModuleWindow`; `titleIntSettings` stays data |
| deletes | `tools/source_policy.py` |
| size | GTE 86; accepted 0; codemap 95. Draw distance is outside this pass. |

60 fps for CTR, Vagrant, C-12, Crash 1/2/3 and Spider-Man 1/2 is feature work after this pass. This pass keeps each title's current interpolation, except Spyro 1, which keeps it through producers.

## 5. Pass order

### (a) psxport adds its final interface (one brief)

Continue from the uncommitted M8a tree; do not restart it.

1. Finish M8a. `FrameLoopShell::bound_` becomes a member of the shell's owner, still rebinding only when the running Core changes. Delete the neutral-runtime branches in FramePresenter and the pacer.
2. Add the section 3.1 interface in `psx::game`: `TitleSessionState`, `titleCommand`, `TitleAudio`, `TitleDevWarp`, `startProgram` + `Crt0Boot`, `deliverPad`, `packetPool`, `residentText`, `displayHeight`, `interpolates`, `stateProducer`. Each needs a unit test through the stub runtime.
3. Add the stock-library classes (`StockLibcd`, `StockLibgpu`, `StockLibgte`, `StockVSync`, `StockDmaCallbacks`, `StockPadBuffers`), built from the code that today reads `PlatformHlePlan` and the layouts.
4. Typed config, done at the start of the c12 brief: one immutable `psx::config` object built by the config owner from the environment and reached through `Game`. A setting the player changes at runtime (`fps60` from the mods UI) is not config: it is a member of its owner, reached through `Game`. Titles read only these two.
5. `psx::cpu::Gte` with `core.gte()` as the one bind rule; `GpuDevice&` reached through `Game`/`Core`; Lucent `info`/`warn`/`error`/`debug`.
6. The public header set of section 3.5 is fixed now; from its pass on, a title includes only that set (each title review checks it). CMake enforces it in (c), after `Core` stops holding `gpu/` and `cpu/` internals by value.

The old surfaces stay until (c), except the `GameRuntime` virtuals a new one replaces (`createContext`/`destroyContext`, `bootInit`, `guestProgramImage`, `platformHlePlan`, `guestPadBufferLayout`, `guestCdStreamCallbackLayout`, `guestPacketPoolWindows`, `replCommand`, `controlCommand`): (a) deletes them, since keeping both is a second path. `LegacyGameRuntimeAdapter` answers the new virtuals for `GameConfig` titles. (a) is pushed together with c12's pass.

### (b) titles, one brief per repo

| # | repo | why here |
|---|---|---|
| 1 | c12 | Smallest repo that uses five conversions plus GTE. It sets the brief and `features.md` template. |
| 2 | Tomba2Engine | Uses every conversion. The legacy adapter, scheduler and CD leave psxport here, so an interface gap shows up early. |
| 3 | spyro | The most-played title; Tomba has shaken out the interface by now, so Spyro's pass is renderer work on a stable API. |
| 4 | megamanx4 | The other legacy user. After it, `GameConfig`/`GameHooks` have no callers. |
| 5 | toystory2 | Record with producers, plus the keyed-only RE. |
| 6 | crashbash | Record with producers, plus the launcher fold. |
| 7 | crash | Crash 3 Gte to Record. |
| 8 | spider1 | Two Gte to Record moves. |
| 9 | vagrant | Record migration. |
| 10 | ctr | Record migration. |
| 11 | tekken3 | Record migration. |

Code that moves from psxport into a title (the Tomba scheduler and CD) is cut from psxport in that title's brief. It never exists twice.

### (c) psxport deletes what no title uses (one brief)

- **Interface.** The legacy layer (`game_iface.*`, `legacy_game_hooks.h`, `legacy_game_config.h`, `game_hooks_opt.*`, `Core::cfg`/`hooks`/`gameCtx`, every `if (core.cfg)`); the section 3.2 bags; the old `GameRuntime` members; the old free functions (`gte_*`, `GTE_*`, `cfg_log*`, `cfg_on/int/str/dbg`, CVar externs).
- **Record present stands alone** (first, checked with recordcheck t=1 on the migrated Record titles):
  - `PresentLedger` moves to the presenter; `PresentInputs` come from the device display area and `displayHeight()`.
  - Record and Device pictures feed `build_present_image` with no `upload_vram`/`render_geom`; drop `rq.flush` and RqItem capture for Record cores.
  - Keep 24bpp/FMV and `present_fade_rgb`. `DisplayPassGuard` already exists.
- **Renderer.** Everything in section 3.3's delete list; `RenderPath` becomes `{Record, Device}`; delete `GpuDevice::sInstance` and the `s_*` macros; then split `gpu_vk.cpp` (it fits two files only after the Native code is gone).
- **Internal-only conversions.** These touch no title: device classes, debug singletons, dead functions, block-scope externs, and non-Beetle `extern "C"`. psxport's own GTE, config and logging callers move as well.
- **Enforcement.** `Core` holds `gpu/`/`cpu/` internals behind its own owners so `core.h` stops exposing them; apply the PRIVATE split; empty psxport's `accepted.txt` (1,905 lines); trim `PSXPORT_CAPS`; align `.clang-tidy`.
- **Docs.** psxport `features.md`, the slim codemap, `presentation.md`; delete `one-renderer.md` and `presentation-contract.md`.

### (d) push as each pass lands

- **psxport (a) with c12.** Title CI shallow-clones psxport `main`, so psxport (a) goes first, then c12 in the same session.
- **Each title after its pass.** The repo is pushed once its section 6 checks are green.  CI was red in 11 of 12 repos on 2026-10-14 for causes unrelated to this pass (psxport: undeclared submodules, fixed in 336cbe60; c12: `c12_frame_driver` needs a video device on the runner, `c12_cpp_style`; vagrant: launcher tests need provisioned inputs; Tomba: `cpp_policy` at 510 s and link isolation; toystory2, spider1, megamanx4: configure; tekken3: bootstrap). Breakage during the pass is accepted: CI is made green across all 12 repos once, after (c) and before the play check, each cause fixed at its owner.
- **Unmigrated titles.** From (a) until its own pass lands, an unmigrated title cannot build from source, locally or in CI; its CI stays red in that window and nobody rebuilds its `build/`. This is the cost of no compatibility layer, and it shrinks one repo per brief.
- **psxport (c)** is pushed after the last title, with a rebuild of all 12 repos against it.

## 6. Checks

- **Per repo, at the end of its brief:**
  - Build with Clang.
  - Run the unit tests of the touched code, through shipping seams.
  - Boot-smoke every title in the repo: the built binary, hidden window, `PSXPORT_NATIVE_FRAMES` set, reaching that title's documented boot milestone. A repo with a picker smokes it too.
  - Record titles also get recordcheck at t=1 and 1x 4:3, with `mismatched=0` as an absolute oracle match, not a before/after comparison.
  - No full sweeps; untouched repos only have to build.
- **Once, after (c): the real-window play check.**
  - Run the built binary in a visible vsynced window on the user's session.
  - Spyro 1/2/3 first, then every other title.
  - At least 60 s of gameplay per title, reached through its warp or pad replay, never by steering.
  - The numbers come from the RunEndReport.
  - **Pass**, absolute only:
    - 0 audio underruns (`needsReprime`) and 0 drops (`shouldDrop`) after boot.
    - Missed-vsync intervals (over 1.5 periods) at most 0.5%, and never 2 in a row.
    - Stable frame order: the shown record sequence never repeats or goes backwards, and in-betweens are neither dropped nor doubled.
  - **No reference.** No run of the current port is a reference, and "an earlier run showed it too" never passes a failure.
  - **Failures.** Each failure gets one issue file in the owning repo and is fixed in its own brief. A failure does not hold the push, because nothing earlier is a trusted state.

## 7. Risks and what is not mechanical

| item | why it is not mechanical | risk |
|---|---|---|
| Spyro 1 to Record | It needs RE of the `0x8001ED5C` tail, culls and HUD anchors, and five producer families moved out of about 40k lines of native render | It is the long pole; the titles after it wait unbuildable while it runs. Draw order becomes retail OT order, so pops the depth order hid (issue 0120) come back. That is the intended oracle-exact result. |
| Gte to Record (S3, T1, SM1, SM2, C3) | The culling was widened for the Gte centre | Margin pop-in at 16:9. Spyro 3 loses 60 fps unless its terrain producer lands in the same brief. |
| keyed-blend removal | TS2 0040/0041 and Tomba 0033 need RE to become producers | Any object left keyed-only shows its shown-record position in the in-between, which is visible judder. |
| stock HLE classes replace `PlatformHlePlan` | Same functions at the same addresses, but now bound by the title | A missed binding changes CD, VSync or DMA timing. X4's DMA-3 STR path changes branch. |
| Tomba scheduler and CD out of psxport | stock versus non-stock libcd is decided per function in Ghidra | The DMA callback comes from the native registry instead of the guest slot. |
| GTE rewrite (about 1,460 sites) | clang tooling does the rewrite, but sites with no `Core` in scope need threading by hand | A missed rebind writes one Core's GTE into another's (picker panels). |
| typed config | `OtAttr::trackStore` is hot | A slower load shows up as missed vsyncs in the play check. |
| psxport's `accepted.txt` (1,905 sites) | Mostly mechanical moves into namespaces and owners | Size: this is the bulk of (c). |
| the migration window | an unmigrated title cannot build against psxport (a) | Each title is unbuildable from source, and red in CI, from (a) until its own pass lands. |
| headless checks | Boot smoke and recordcheck cannot see lag, flicker or audio | Covered only by the single end play check. |

## 8. User-visible changes

- Spyro 1 draws in retail order.
- Former Gte titles get canvas margins at 16:9.
- The Crash Bash launcher is replaced by the shared first-run setup.
- These instruments are gone with no replacement: texwatch, clutwatch, gp0raw, zfight and the objid overlay. recordcheck and the producer census remain.

# Features — psxport

Each feature is a chain of hops from the product loop to the code that does the work. A hop row names
its owner as `path:Symbol` (paths under `runtime/`). A title joins a hop as `psxport F-X.n` in its own
`docs/features.md`; the **title** column names the `psx::game::GameRuntime` virtual or capability the
title answers there. A hop with no title entry runs on the framework alone.

## F-BOOT — from launch to the title's first instruction

| hop | owner | title |
|---|---|---|
| 1 | `psx/catalog/title_catalog.h:TitleCatalog`, `psx/host/title_disc.cpp:TitleDisc` — the titles a product offers and the disc each reads (`discEnvVar`, `PSXPORT_DISC`, saved choice) | `createCatalog`, `TitleIdentity` |
| 2 | `psx/host/executable_provisioner.cpp:provisionExecutable`, `psx/catalog/title_selection.cpp:selectDiscExecutable` — the authenticated PS-X EXE from the disc | |
| 3 | `psx/host/product_host.cpp:ProductHost::run` — one `Game` per picked title on the shared window | |
| 4 | `psx/host/title_session.cpp:TitleSession::boot` → `GameRuntime::admitExecutable` — the EXE header and resident image | `admitExecutable` |
| 5 | `TitleSession::boot` → `GameRuntime::startProgram`; `psx/game/crt0_boot.h:Crt0Boot::run` for crt0 replay | `startProgram` |
| 6 | `TitleSession::boot` → `GameRuntime::registerOverrides`; `psx/game/game.cpp:Game::wireRuntimeMembers` → `createSession`, `createFrameDriver` | `registerOverrides`, `createSession` |

## F-TURN — one logic frame

| hop | owner | title |
|---|---|---|
| 1 | `psx/host/title_session.cpp:TitleSession::step` → `psx/frame/frame_loop_shell.cpp:FrameLoopShell::step` | |
| 2 | `FrameDriver::stepFrame` — one guest turn and exactly one `FramePresenter::commit` | `createFrameDriver` |
| 3 | `psx/platform/coro.h:Coro` — the continuation a title's own task scheduler runs on, stepped by its frame driver | session member |
| 4 | `psx/boot/run_end_report.cpp:RunEndReport::emit` → `GameRuntime::reportRun` | `reportRun` |
| 5 | `psx/frame/frame_presenter.cpp:FramePresenter::commit` → `GameRuntime::pacePresentation` (default `psx/frame/frame_pacer.cpp:FramePacer::paceSubframeFields`) | `pacePresentation` |

## F-INPUT — host pad to guest

| hop | owner | title |
|---|---|---|
| 1 | `psx/input/pad_input.cpp:Pad::serviceFrame` → `GameRuntime::deliverPad`; default BIOS pad path, `psx/input/stock_pad_buffers.cpp:StockPadBuffers` for buffer-writing titles | `deliverPad` |
| 2 | `psx/input/pad_input.cpp:Pad::currentPhase` → `GameRuntime::inputPhase` — the phase a `.pad` replay matches on | `inputPhase` |
| 3 | `psx/input/pad_record_replay.cpp:PadRecordReplay` — `.pad` record and replay; force/hold, REPL drive, host suppression and replay each win over the one before | |
| 4 | `psx/input/host_input.cpp:HostInput` — the one SDL drain: key state, the overlay, a window close on the `QuitScope`; no window means no host buttons | |
| 5 | `psx/input/sio_pad.cpp` — the 4-byte digital packet per VBlank | |

## F-PICK — the in-window title picker

| hop | owner | title |
|---|---|---|
| 1 | `psx/host/panel_sessions.cpp:PanelSessions::takeLogo` → `GameRuntime::panelLogo` | `panelLogo` |
| 2 | `psx/host/picker_session.cpp:PickerSession::run`, `psx/host/picker_runtime.h:PickerRuntime` — one live panel per title, one window | |

## F-RENDER — guest GPU work to the screen

| hop | owner | title |
|---|---|---|
| 1 | `psx/present/render_mode.h:RenderMode`, `psx/config/config_vars.h:render_path` — `record` (default) or `device`, installed by `Game::wireRuntimeMembers` | |
| 2 | `psx/debug/ot_attr.cpp:OtAttr::refreshPool` → `GameRuntime::packetPool` — the packet pool the census attributes | `packetPool` |
| 3 | `psx/present/display_presenter.cpp:DisplayPresenter::presentedHeight` → `GameRuntime::displayHeight` | `displayHeight` |
| 4 | `psx/gpu/gpu_vk_present.cpp:presentInputs` → `GameRuntime::guestPictureIsNativeWidth` | `guestPictureIsNativeWidth` |
| 5 | `psx/gpu/gp0_record_tap.h:Gp0RecordTap`, `psx/present/frame_record.h:FrameRecord` — the device's GP0 work recorded per logic frame | |
| 6 | `psx/present/display_presenter.cpp:DisplayPresenter::build` — the shown record, rasterised; `show` presents it | |

## F-WIDE — widescreen

| hop | owner | title |
|---|---|---|
| 1 | `psx/present/guest_widescreen_projection.cpp:requestedAspect` → `GameRuntime::guestWidescreenProjection` — the canvas aspect and margins | `guestWidescreenProjection` |
| 2 | `psx/present/guest_widescreen_projection.h:GuestProjectionPlan` — the margin a title widens its own guest culls by | title render code |

## F-FPS60 — 60 fps from saved state

| hop | owner | title |
|---|---|---|
| 1 | `psx/frame/frame_presenter.cpp:composeGuarded` → `GameRuntime::stateProducer` — the render for a producer key | `stateProducer` |
| 2 | `psx/frame/frame_presenter.cpp:FramePresenter::commitSealed` → `GameRuntime::sealedFrameIsCut` — no in-between across a cut | `sealedFrameIsCut` |
| 3 | `psx/present/frame_composer.h:composeFrame` — the in-between: producer renders at `t` on the shown record | |
| 4 | `psx/game/game.cpp:Game::wireRuntimeMembers` → `GameRuntime::interpolates`; `psx/debug/mods.h:Mods::fps60` | `interpolates` |

## F-FMV — movies

| hop | owner | title |
|---|---|---|
| 1 | `psx/movie/native_fmv.cpp:Fmv::play` — a movie a title starts, decoded and presented until it ends | title movie owner |
| 2 | `psx/movie/fmv_decode.cpp` — STR/MDEC decode | |

## F-CD — disc reads

| hop | owner | title |
|---|---|---|
| 1 | `psx/cd/stock_libcd.cpp:StockLibcd::bind` — stock libcd entries bound at the title's addresses; `GuestInterruptLibcd` when the guest's INT1 delivers readiness | `registerOverrides` |
| 2 | `psx/cd/cdc_native.cpp:CdController` — the CD controller over `psx/cd/disc.cpp:Disc` | |
| 3 | `psx/core/guest_code_module.cpp:publishLanding` → `GameRuntime::guestCodeModuleWindow` — a module landed in its window | `guestCodeModuleWindow` |
| 4 | `psx/cd/cd_stock_read_completion.cpp:announceStockReadLanding` → `GameRuntime::stockCdReadLanded` | `stockCdReadLanded` |

## F-AUDIO — SPU and XA

| hop | owner | title |
|---|---|---|
| 1 | `psx/audio/spu_audio.cpp:SpuAudio::frameEx` → `GameRuntime::audio` (`TitleAudio::mixFrame`) | `audio` |
| 2 | `psx/audio/spu_device.cpp`, `psx/audio/xa_stream.cpp:XaStream` — the SPU and the XA streamer | |

## F-LOAD — loading removal

| hop | owner | title |
|---|---|---|
| 1 | `psx/cd/stock_libcd.cpp:StockLibcd::read` — a stock `CdRead` lands whole and synchronously | |
| 2 | `psx/frame/frame_pacer.cpp:FramePacer::plan` — no host sleep while `Game::fastForwarding` | |

## F-SAVE — cards and states

| hop | owner | title |
|---|---|---|
| 1 | `psx/game/game.cpp:Game::hostIdentity` → `GameRuntime::hostIdentity`; `psx/platform/memcard.h:Memcard` | `hostIdentity` |
| 2 | `psx/state/machine_state.cpp:MachineState::titlePort` → `GameRuntime::nativeState` | `nativeState` |
| 3 | `psx/state/state_file.h:StateFile` — the state file and its required sections | |

## F-DEBUG — control channel and dev menus

| hop | owner | title |
|---|---|---|
| 1 | `psx/debug/debug_commands.cpp:DebugCommands::run` → `psx/game/title_command.cpp:runTitleCommand` → `GameRuntime::titleCommand` | `titleCommand` |
| 2 | `ui/warp_control.cpp:WarpControl::titleWarp` → `GameRuntime::devWarp` | `devWarp` |
| 3 | `ui/menu_readouts.cpp:MenuReadouts::update` → `GameRuntime::worldReadout` | `worldReadout` |
| 4 | `psx/debug/producer_census.h:ProducerCensus` — primitives per producer, keyed and rendered | |
| 5 | `psx/debug/bug_report_session.h:BugReportSession` — the in-app bug report | |

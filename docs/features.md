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
| 5 | `psx/frame/frame_presenter.cpp:FramePresenter::commit` → `GameRuntime::presentDeadline` (default `psx/frame/frame_pacer.cpp:FramePacer::subframeDeadline`) → `psx/present/display_presenter.cpp:DisplayPresenter::showAt` | `presentDeadline` |

## F-INPUT — host pad to guest

| hop | owner | title |
|---|---|---|
| 1 | `psx/input/pad_input.cpp:Pad::serviceFrame` → `GameRuntime::deliverPad`; default BIOS pad path, `psx/input/stock_pad_buffers.cpp:StockPadBuffers` for buffer-writing titles; a title whose `GameRuntime::padPorts` is 2 gets port 2 from the second physical controller or the control channel's `p2` word (live input only; `.pad` record and replay serve port 1) | `deliverPad`, `padPorts` |
| 2 | `psx/input/pad_input.cpp:Pad::currentPhase` → `GameRuntime::inputPhase` — the phase a `.pad` replay matches on | `inputPhase` |
| 3 | `psx/input/pad_record_replay.cpp:PadRecordReplay` — `.pad` record and replay; host input (keyboard, gamepads and the control channel's virtual gamepad), force/hold, host suppression and replay each win over the one before | |
| 4 | `psx/input/host_input.cpp:HostInput` — the one SDL drain: key state, the overlay, a window close on the `QuitScope`; no window means no host buttons | |
| 5 | `psx/input/sio_pad.cpp` — the 4-byte digital packet per VBlank | |

## F-PICK — the in-window title picker

| hop | owner | title |
|---|---|---|
| 1 | `psx/host/panel_sessions.cpp:PanelSessions::takeLogo` → `GameRuntime::panelLogo` (default: `drawnLogo` of `logoTextures` while `showsOwnLogo`); `PanelSessions::logo` → `GameRuntime::showsOwnLogo` hides it on any panel whose picture shows it; `PanelLayout::logoRect` sizes it | `showsOwnLogo`, `logoTextures` (`panelLogo` when not textured) |
| 2 | `psx/host/picker_session.cpp:PickerSession::run`, `psx/host/panel_sessions.cpp:PanelSessions::advance` — only the focused panel runs, one paced step per iteration; `psx/host/picker_runtime.h:PickerRuntime`; one window | |

## F-RENDER — guest GPU work to the screen

| hop | owner | title |
|---|---|---|
| 1 | `psx/present/render_mode.h:RenderMode`, `psx/config/config_vars.h:render_path` — `record` (default) or `device`, installed by `Game::wireRuntimeMembers` | |
| 2 | `psx/debug/ot_attr.cpp:OtAttr::refreshPool` → `GameRuntime::packetPool` — the packet pool `otattr` attributes | `packetPool` |
| 3 | `psx/present/display_presenter.cpp:DisplayPresenter::presentedRect` → `GameRuntime::displayHeight` | `displayHeight` |
| 4 | `psx/render/render_present.cpp:RenderState::presentInputs` → `GameRuntime::guestPictureIsNativeWidth` | `guestPictureIsNativeWidth` |
| 5 | `psx/gpu/gp0_record_tap.h:Gp0RecordTap`, `psx/present/frame_record.h:FrameRecord` — the device's GP0 work recorded per logic frame | |
| 6 | `psx/present/display_presenter.cpp:DisplayPresenter::build` — the shown record, rasterised; `show` presents it | |

## F-WIDE — widescreen

| hop | owner | title |
|---|---|---|
| 1 | `psx/present/guest_widescreen_projection.cpp:requestedAspect` → `GameRuntime::guestWidescreenProjection` — the canvas aspect and margins | `guestWidescreenProjection` |
| 2 | `psx/present/guest_widescreen_projection.h:GuestProjectionPlan` — the margin a title widens its own guest culls by | title render code |

## F-FPS60 — 60 fps from a replayed draw

| hop | owner | title |
|---|---|---|
| 1 | `psx/frame/frame_presenter.cpp:FramePresenter::presentRecords` → `GameRuntime::replayFrame` — the title's draw section replayed for each in-between (`present/replay_frame.h:ReplayFrame::at`) | `replayFrame` |
| 2 | `psx/frame/frame_presenter.cpp:FramePresenter::commitSealed` → `GameRuntime::sealedFrameIsCut` — no in-between across a cut | `sealedFrameIsCut` |
| 4 | `psx/game/game.cpp:Game::wireRuntimeMembers` → `GameRuntime::interpolates`; `psx/debug/mods.h:Mods::fps60` | `interpolates` |

## F-FMV — movies

| hop | owner | title |
|---|---|---|
| 1 | `psx/movie/native_fmv.cpp:Fmv::play` — a movie a title starts, decoded and presented until it ends | title movie owner |
| 2 | `psx/movie/fmv_decode.cpp` — STR frame assembly and BS decode through `psx/movie/mdec_device.cpp:MdecDevice`, XA through `psx/audio/xa_adpcm.cpp` | |

## F-CD — disc reads

| hop | owner | title |
|---|---|---|
| 1 | `psx/cd/stock_libcd.cpp:StockLibcd::bind` — stock libcd entries bound at the title's addresses; `GuestInterruptLibcd` when the guest's INT1 delivers readiness, its synchronous read's completion due after the drive time or, for a title whose loads carry no drive wait, at once (`Completion::timing`, `ReadTiming`); a command run here latches the interrupt edge it raises (`Core::irqStatLatch`), as a guest controller write does (issue 0186); a Pause or Stop sent while the previous one is still being received executes that one first, as libcd waits out BUSYSTS (`CdController::issueCommand`, issue 0200); an owed read completion becomes current only with no response queued ahead of it and outside an interrupt dispatch (`psx/cd/cd_ready_delivery.cpp:OwedCompletionHold` around `Hle::irqPoll`), so the framework delivers it before the guest's own CD handler, which has no libcd read state for it, can acknowledge it unseen (issue 0204) | `registerOverrides` |
| 2 | `psx/cd/cdc_native.cpp:CdController` — the CD controller over `psx/cd/disc.cpp:Disc`, opened when the title boots; CHD hunks decoded ahead by `psx/cd/hunk_reader.cpp:HunkReader` | |
| 3 | `psx/core/guest_code_module.cpp:publishLanding` → `GameRuntime::guestCodeModuleWindow` — a module landed in its window | `guestCodeModuleWindow` |
| 4 | `psx/cd/cd_stock_read_completion.cpp:announceStockReadLanding` → `GameRuntime::stockCdReadLanded` | `stockCdReadLanded` |

## F-AUDIO — SPU and XA

| hop | owner | title |
|---|---|---|
| 1 | `psx/audio/spu_audio.cpp:SpuAudio::frame` → `GameRuntime::audio` (`TitleAudio::mixFrame`) → `psx/audio/host_audio_output.cpp:HostAudioOutput::Stream` | `audio` |
| 2 | `psx/audio/spu_device.cpp`, `psx/audio/xa_stream.cpp:XaStream` — the SPU and the XA streamer; `psx/audio/spu_beetle.cpp` runs the SPU to the guest's emulated time at every register and DMA access, and to the next sample before a KON/KOFF latch written with no time between is rewritten (issue 0183) | |

## F-LOAD — loading removal

| hop | owner | title |
|---|---|---|
| 1 | `psx/cd/stock_libcd.cpp:StockLibcd::read` — a stock `CdRead` lands whole and synchronously; a landing inside the runtime's code-module window activates it like a DMA3 transfer (`announceStockReadLanding` → `code_module::publishLanding`) | |
| 2 | `psx/frame/frame_pacer.cpp:FramePacer::plan` — no host sleep while `Game::fastForwarding` | |

## F-SAVE — cards and states

| hop | owner | title |
|---|---|---|
| 1 | `psx/game/game.cpp:Game::hostIdentity` → `GameRuntime::hostIdentity`; `psx/platform/memcard.h:Memcard` — card path from `PSXPORT_CARD` for every title, else the title's default | `hostIdentity` |
| 2 | `psx/state/machine_state.cpp:MachineState::titlePort` → `GameRuntime::nativeState` | `nativeState` |
| 3 | `psx/state/state_file.h:StateFile` — the state file and its required sections | |

## F-DEBUG — control channel and dev menus

| hop | owner | title |
|---|---|---|
| 1 | `psx/debug/debug_commands.cpp:DebugCommands::run` → `psx/game/title_debug_menu.cpp:TitleDebugMenu::run` over `GameRuntime::debugMenu` — the title's verbs, typed parameters and refusals; `help` lists them | `debugMenu` |
| 2 | `ui/title_debug_rows.cpp:TitleDebugRows` — the Debug tab's sections, readouts, parameter and run rows and the About lines, generated from the same `TitleDebugMenu`; a run row sends the line its selections spell through hop 1 | |
| 3 | `psx/game/title_debug_menu.cpp:TitleDebugMenu::soundTest` — `music <track>`, `music-stop` and the now-playing readout over `TitleAudio` track names | `TitleAudio::trackName` |
| 3a | `psx/game/title_debug_menu.cpp:TitleDebugMenu::warp` — the title's `WarpCatalogue`: picker labels (`homeworld`, `level`, `arrival`) and a tree of `WarpPlace`s whose leaves are arrivals at one depth, built once from guest data when the title's builder first returns places. `warp <path>` takes names (any case) or indices joined by `/` (`warp Artisans/Stone Hill/portal`, `warp 0/1/1`); `warp ?` or a path ending in `?` lists the tree under it; a refusal names the valid children. An arrival only arms the title's frame-boundary request, which goes through the game's own load/transition owner; the `[warp] <path>: <reply>` log line carries the title's own reply (a refusal included), so an arrival is proven by the title's apply line, not by it. The verb sits in no section: the verbs and readouts a title registers after `warp(...)` stay in the section open before it. The Debug tab shows one cascading picker per level and Go (`TitleDebugRows::warpGo`, which sends the numeric path through hop 1) | `debugMenu` (`warp(...)`) |
| 5 | `psx/debug/bug_report_session.h:BugReportSession` — the in-app bug report: the reference frame sealed at `FramePresenter::commit`, the last second of presents from `render/present_clip.h:PresentClip`; only the Game holding player input takes P / '.' / B (`input/pad_input.cpp:Pad::pollHostInput`) from the process's one `HostInput` | |
| 6 | `psx/debug/debug_commands.cpp` `key`/`text`/`mouse`/`gamepad`/`touch` → `input/input_injection.cpp:InputInjection` — SDL events pushed into the window's queue on the main thread; a modifier key sets SDL's modifier state, so a chord is the modifier `down`, the key, the modifier `up`; `key`/`mouse`/`touch`/`gamepad` taps last the player Game's input turns (`Pad::pollHostInput`), the pause loop's included; the guest-pad verbs `tap`/`press`/`release`/`hold` press the virtual gamepad through `Pad::injectButtons`, the one press entry for the channel and title drivers alike, a `tap` lasting pad frames the guest is served (`Pad::serviceFrame`), so a paused run holds it; a press is refused while a pad replay owns input or another Game (a picker preview panel) holds the player's input, and `PSXPORT_PAD_NOPAD` leaves the virtual pad open | |

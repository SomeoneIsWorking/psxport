# Consolidation plan: psxport and psx-spyro

## How the plan is ordered

- **Owners first.** A group that only *adds* a shared owner (a header, constant or helper) comes before the groups whose files hold the copies. The owner of each copy deletes it in its single pass over that file.
- **Breaking changes land with their callers.** When an owner's API changes shape (rename, signature change, removal), that group edits every caller in the same change, in every title repo. psxport and the touched titles are pushed together, then CI is checked.
- **One owner per file.** Each file belongs to exactly one group, which applies every finding for that file. The only exceptions are listed in [Files edited by two groups](#files-edited-by-two-groups); each is a few lines forced by an API break.
- **Build lists and docs.** CMake source lists and per-row edits to `docs/codemap.md`, `docs/project-state.md` and `docs/re-frontier.md` are made by whichever group adds, moves or deletes the file, in the same commit.
- **Phases.**
  - Phase A can start now.
  - Phase B touches the files in the present split / record-raster work (frame_presenter\*, display_presenter\*, gpu_vk_present\*, gpu_vk_record\*, record_raster\*, record.frag, native_fmv\*, psx/host/picker\*, pane_composite\*), or depends on a group that does. **All Phase B groups run after that work lands.**
- **Proving it works.** No tests are run. Each group ends with headless runs of the built Spyro binary using its hidden-window mode, `tools/drive.py`, the title `warp` commands, REPL/debug-server `shot` and `dumpram`, and save states. Each phase ends with one real-time operator play check on Spyro 1, 2 and 3 (headless runs miss lag, flicker and audio). Every group that changes psxport also smoke-boots every other title to its title screen.

---

## Phase A (can start now)

### A1. Lucent: text and config-variable primitives
- **Owner:** `vendor/lucent` (maintained repo; changes committed upstream).
- **Files:** `include/lucent/text.h`, `src/text.cpp`, `include/lucent/cvar.hpp`, `src/cvar.cpp`.
- **Moves:** `cvar::detail::trim` becomes `lucent::text::trim(string_view)`.
- **Adds:**
  - `lucent::text::json_quote` (Lucent naming), with the full escaping that `override_differential_report.cpp::jsonString` does today.
  - Four cvar extensions: `Layer::Runtime`, per-layer queries (`has`, `at`, `layer_string`, `clear`), `Persistence::RunOnly`, and `Options::env_name` for an exact environment name; a value of any type that does not parse is logged through Lucent and ignored.
- **Deletes:** `http.cpp`'s private header trim, folded into `lucent::text::trim`; header names and values now also lose edge CR, LF, VT and FF, not only SP/HTAB (lines split on CRLF, so only malformed headers differ).
- **The one implementation:** text trim, JSON quoting and layered config variables all live in Lucent.
- **Spyro proof:** psxport and Spyro build; Spyro 1 boots to its title screen headless.

### A2. psxport shared vocabulary (additive only)
- **Owner:** the leaf headers listed below. Callers move onto them in later groups.
- **Files and what each gains:**
  - `psx/cpu/r3000.h`: all 32 `Gpr` entries, `gprName()`, COP0 `kCop0Status/Cause/Epc`, and RAII `GuestContextScope` (saves R3000, restores on scope exit, has an explicit `restore()`).
  - `psx/cpu/gte_registers.h`: `kSqr`, `kOp`, `kLzcs`, `kLzcr`, `kMvmvaLlmIr`, `kMvmvaRtV0Tr`, `kMvmvaLcmIr`, and the `RotationWords` type.
  - `psx/cpu/gte.h`: `loadRotation` and `storeRotation`.
  - `psx/cpu/cpu_divide.h/.cpp`: pure `quotient()` and `remainder()`; `divide()` is rebuilt on them.
  - `cpu/guest_call_site.h`: `kJalReturnOffset` and `GuestCallSite::fromJal`.
  - New `cpu/native_function.h`: the `NativeFunction` type.
  - `psx/core/guest_abi.h`: `PreservedReturnAddress` and `GuestFrameScope`, taken from Spyro's `native_execution.h`.
  - New `psx/core/guest_memory_map.h` (namespace `psx`): `kMainRamBytes`, `kRamMirrorSpan`, `kPhysicalMask`, `kScratchpadPhysicalBase/Bytes`, `kBios*`, `kHardware*`, `kMainRamEnd`, `physical()`, `mainRamOffset()` and `kseg0Address()`.
  - New `psx/platform/host_clock.h`: steady_clock ns and ms helpers.
  - `psx/present/vram_rect.h`: `kVramWidth/Height`.
  - `psx/gpu/vram_pixel.h`, exported to titles: a constexpr TexPage/CLUT word decode and a CPU texel sampler using hardware addressing (`u>>(2-mode)`, nibble `(u&3)*4`).
  - `psx/present/gp0_command.h`: `signedAxis` and the `kFlag*` bits made public, polygon opcode builders, and tag/byte-size helpers derived from `packetWordCount`. Its private `kVramWidth/Height` are deleted in favour of `vram_rect.h`, and its narration is trimmed.
  - `psx/config/number_text.h/.cpp`: `splitList(text, separators)`, `parseUnsignedList(text, base)`, and base-0 parsing in `parseSigned`.
  - `psx/frame/field_rate.h`: `fieldIntervalMs(rate, fields, parts)` and one host-screen rate constant. Deletes the test-only `FIELD_RATE_PAL_MILLIHZ`, the stale `rec_host_turn_register` text and the narration.
  - `psx/present/blend.h`: `lerpWord`.
- **Spyro proof:** builds; Spyro 1 boots to title. Behaviour does not change.

### A3. Execution core, config, debug and input, plus their Spyro and cross-title consumers
One group because its API breaks reach every caller. psxport commits come first, the title commits that use them follow, and everything is pushed together.

- **Owner modules and their files:**
  - psxport `runtime/cpu/*`.
  - `psx/core/*` (except `overlay_glue.*`, which is B1).
  - `psx/cpu/beetle/gte_beetle.cpp`.
  - `psx/hle/*`, `psx/boot/*`, `psx/state/*`.
  - `psx/platform/{dma_irq.h, dma_callbacks.h, io_peripherals.*, irq_edge.h, spin_detector.h + new spin_detector.cpp, watchdog.cpp, coro.h, card_syscall_log.*, memcard.*}`.
  - `psx/frame/{timing.*, emulated_time.*, census_frame.h (deleted), field_turn.*, control_surface_limits.h (moves to debug)}`.
  - `psx/config/*` (except `number_text`), `psx/debug/*`, `psx/input/*`.
  - `psx/audio/spu_device.cpp`.
  - `psx/game/{game.h, game_runtime.*, crt0_boot.*, psx_exe_image.*, disc_executable.cpp, title_command.cpp}`, `psx/catalog/title_selection.cpp`.
  - `tools/crt0_extract.cpp`, `tools/oracle/oracle_snapshot.cpp`.
  - All non-render Spyro code: `psx-spyro/game/core/*` (except `title_logo*`, which is A4), `titles/spyro1/core/*` (except the `hud` render producers and `hud_anchor`, which are B4), `titles/spyro2|3/core/*`, and the Spyro `CMakeLists.txt`.

- **Merged findings and their single implementation:**
  - **Guest calls.**
    - `ResumableGuestCall` owns resuming a guest call across host turns. Delete `native_dispatch`'s `ResumedCall`/`resumeUntilExit`/`callOriginalResumingToExit`/`callOriginalToReturnResuming` family, `callOriginalUntilExit`, and the NativeKey overload. Callers move: `OverrideDifferential::shadow`, Vagrant's `dynarec_dispatch`, and TS2's `sound_bank`.
    - `callGuestNow` is the only call that spells the current-turn budget. Add a result-returning current-turn form beside it and delete `dispatchGuestToReturn0`.
    - `GuestContextScope` replaces every hand-written `R3000 saved` copy in `host_turn`, `hle` and `hle_interrupt`.
    - Add a `callGuestNow(GuestCallSite)` overload and `callOriginalOrPropagate`/`dispatchGuestOrPropagate`. These replace Spider-Man's `guest_execution.cpp` pair and Spyro's `JalSite`, `callGuestJumpedFrom` and the 10 inline `completeOrPropagate(callOriginal…)` spellings.
    - `ExecutionControl::consume(unstatedPc, cycles)` replaces the three executor copies and `completeHostLeaf`'s copy of that rule.
  - **Interrupts (owner: `Hle`).**
    - `Hle::runAsInterrupt` plus the interrupt guard. `deliverEvent` gains the exception stack it is missing.
    - `canDispatchInterrupt` is the one readiness predicate; `serviceHostTurn` and `irqPoll` use it.
    - One `irqDeliverable`/disarm helper.
    - Core gains `raiseInterrupt(IrqBit)` and `irqPendingEnabled(bit)`, used by SIO, CD-latch, VBlank, SPU and Spyro's `vblank_irq.h` (deleted).
    - `irqStatLatch` moves to `core.cpp`.
    - The jmp_buf layout lives only in `bios_interrupt` (`saveJmpBuf`).
    - One guest-copy helper serves memmove/memcpy/realloc.
    - `readCString` replaces the printf, puts and `deviceFind` loops.
    - Table capacities become `Hle::kEventSlots` and `Hle::kHeapBlocks`.
    - `handleBreak` moves to `execution_services.cpp`.
    - Spyro's handler stack uses the guard and drops the `0x8000E000` copies.
  - **Memory map.**
    - `Core::host_ptr` and `isGuestStorage` own the classification, built on `guest_memory_map.h`.
    - `display_pass_write_guard` calls `isGuestStorage`. This fixes missed mirror writes.
    - `deadStackWindow` folds through the map.
    - Local constants deleted: `machine_snapshot`, `function_reach`, `lightrec_executor`, `psx_exe_image`, `override_differential_contract`, Spyro `canonicalAddress`/`guest::ramOffset`, `guest_packet_pool_windows`.
    - The bare `&0x1FFFFFFF` sites in A3 files call `physical()`.
  - **DMA.**
    - `DmaRegisters` holds `channel[7]` plus the MDEC and SPU cursors.
    - Deleted: `BusShadows`, `DmaChannelShadow`, `s_dmaN_*`, and the DMA fields in the CPU save-state section.
    - `busShadows` and `clockSnapshot` leave `state/`. `ClockSnapshot` holds an `EmulatedTime` state.
    - `doneOwed`/`doneTaken`/`doneAny`/`ackIrq` are deleted. Their pure `ackDicr` rule moves beside `completeDicr`, and the DPCR/DICR constants, `DmaChannel`, `DmaDone` and `DmaRegisters` move into `psx::dma`. Callers updated: `hle_interrupt`, Tomba's `stream_field_turn.cpp` and `sync_native.cpp`, and CTR's `kSpuDmaChannel`.
  - **Clocks.**
    - `kNominalPsxCpuHz` replaces the literals in `execution_exit`, `SpuFieldCadence` (as an alias) and Spyro's 4-second budgets.
    - New `Timing::displayFieldRate()` (decoded from GP1). `ExecutionBudget::currentTurn`, `hSyncTotal`, `stock_vsync` and Spyro's `field_owner` read it. `FramePacer::fieldRateMilliHz` itself is deleted in B3.
    - `HostTurnClock` becomes a class held as a `Core` member, with one `rearm()`. Its source moves to `runtime/cpu`, and its callers move to member calls (including Spyro's `field_owner`).
  - **crt0 and PS-X EXE.**
    - `scanCrt0` (moved beside `Crt0Boot`) is the only source of the boot facts. Delete `auditCrt0`, `Crt0AuditRow`, `crt0Completeness` and every field of `Crt0Image` except the entry. Every title's `kCrt0` shrinks to its entry: Spyro 1/2/3, Crash 1/2/3, Crash Bash, Tomba 1/2, Spider-Man, MMX4 and TS2. `crt0_extract` becomes a dump of the scan. The crt0 types move into `psx::game`.
    - `parsePsxExeImage` is used by `title_selection` and `crt0_extract`.
    - `loadTopLevelPsxExe` serves `admitExecutable`, `loadDiscExecutable` and `bios_load_exec`.
    - `code_module::digest` replaces the boot EXE's FNV identity. Tomba and CTR keep their own catalog hashing.
  - **Vocabulary adoption.**
    - `NativeFunction` replaces both `OverrideFn` typedefs and `HostTurnFunction`.
    - `gprName` and `Gpr` replace `kRegisterNames`, `kReg[32]`, `names[11]`, `kStackPointer`, the bare indices in `crt0_boot`/`guest_call`, and the dead `mipsRegisterName`. The COP0 constants replace `syscall_exception`'s copies.
    - `Gte::saveRaw`/`restoreRaw` replace `MachineSnapshot`'s GTE copy. `gte::` names replace `gte_beetle`'s `kCr*` and `architecturalGte`'s raw indices. Delete `gte_beetle`'s dead `setProjH`/`setProjCenter` writes (the `ProjParams` members themselves go in B1).
  - **Store observation.**
    - One `StoreWatch` in `psx/debug` with a single armed check on the store path. `PSXPORT_WWATCH` and its options fold into it, and the title `stage` word read is removed.
    - Store-observer arming state moves into the executor; `g_armed` and `attachStoreObserver` are deleted.
    - The Lightrec boundary state becomes a member; `thread_local` and the global id are deleted.
    - Spyro's `HandoffStoreObserver` and `StageUpdateObserver` are deleted.
  - **Reports.**
    - `execution_ledger::ledgerLines` owns every counter's text; `reportFallbackTelemetry` is deleted.
    - `lucent::text::json_quote` replaces `jsonEscaped`/`jsonString`.
    - `DifferentialSelector::matches` is the one selector predicate.
  - **Save states.**
    - `saveToFile`/`loadFromFile` are the only save/load paths; `state::save` and `state::load` are deleted.
    - `Fs::readFile`/`writeFile` gain error reporting; `state::readFile`/`writeFile` are deleted.
    - `BlobReader::expectLayout`/`decodeError` replace the copied checks in every section.
    - One ordered section table drives capture and restore, and `kRequired` is derived from it.
    - `BlobWriter`/`BlobReader` move to a neutral location and become the one little-endian codec (pad recording and the oracle tool use it).
    - Beetle selftest machinery deleted; `StateAction` keeps a one-line comment.
  - **Config (owner: `lucent::cvar`).**
    - `psx::config` keeps only the `PSXPORT_*` inventory as `Var<T>`.
    - Deleted: `CVar<T>`, the `env_*` wrappers, `selftest`, `reset_for_test`, `ScopedDiagnosticRun` and `set_from_settings_file`.
    - `typed_config` folds into the registry: NOPACE, REPL and FPS60 become non-persistable vars. APPDIR, XDG_STATE_HOME and HOME are read once.
    - Typed, parsed-once values replace raw strings for every knob whose consumer is in A3.
    - `diagnostic_run` moves to `config/`.
    - The settings file goes through `cvar::load_file`/`save_file`. aspect, ires and fps60 become CVars, the ires_auto compatibility path is deleted, `Mods::ui` (dead) is deleted, and `mods.*` moves out of `debug/`.
    - One `cvar_report_lines` serves `report`, `report_exit_audit` and the debug server's `cvars`.
  - **Debug.**
    - One `DebugCommands` table (over Core, writing to a line sink) holds the shared verbs and one held-pad mask. REPL and DbgServer keep only their transports. `DbgServerInternals` and the `s_*` macros become a `DebugChannel` class.
    - `OtAttr::runCommand` is the one `otattr`. Every OT walk in `gpu_debug`/`repl` uses `OrderingTableCursor`; `gp0CommandLength` is deleted.
    - The help text is generated from the command table: `sbs` is dropped and `tp` moves to Tomba's help.
    - The title-specific `ents`/`node`/`geomblk`/`stage` reads leave the framework and issue 0130 is reopened. The owning title is identified by decomp match before moving them.
    - `Snapshotter` moves to `psx/debug` and is the one guest-RAM writer (RAM plus `.spad`, size from the map). The REPL and debug-server `dumpram`, `RunEndReport`, `FieldTurn` and the pad dump all call it. The frame-N knobs collapse into `SNAP_AT`; GRAMDUMP's deletion happens in B1.
    - One `FrameSchedule` type, which `DisplayPresenter` adopts in B1. The pad-axis shot uses it through `display.shot`.
  - **Input.**
    - `Pad` keeps input resolution only; its instruments and the Tomba trace leave psxport.
    - The full button set is defined in `host_input.h`; delete `kNeutralMask`, `kStart` and the literals in `host_input.cpp`.
    - Delete Pad's dead and forwarding members, plus `PhaseReplay::reportRunEnd`.
    - `PadRecording::append` owns segmentation; `PadSegment::matches` owns the phase-match rule.
  - **Title command line.** `TitleCommandLine` exposes argument words through `CommandArgs`, and every title parses them with `parseUnsigned`. That deletes the parsers in Spyro's `warp`/`gate_debug`, Crash's `parseScene`, CTR's and TS2's `parseLevel`, Tekken 3's `character`/`nextWord`, Vagrant Story's `from_chars` and Crash Bash's `dev_arena` parse.
  - **Frame driver lookup.** `Game::frameDriverAs<T>()` replaces `bootPrefixFrameDriver`, `spyro1::frameDriver`, Crash's `FrameDriver::from` and Crash Bash's `frameDriver`, and `FrameLoopShell::requireDriver` uses the same check. (`frame_loop_shell.cpp` is a B3 file; that call is updated in B3.)
  - **Spyro non-render core.**
    - Spyro-1-only owners move to `titles/spyro1/core`: the native installers, `cd_queue`, `gate_debug`, `guest_globals.h` and `guest_gp.h` (namespaced). `tools/guest_globals.py` HEADERS is updated in A5.
    - One name per address in `guest_globals`, settled against the decomp (`kModels`, the SpecularAndMetal base with offsets, `g_Environment`, and the added `kCameraPosition`). All local redefinitions are deleted.
    - New `guest_functions.h` names each callee entry once (rand, vadd, vsra and the rest), removing names like `kWrapIntoRange` and `kFillVector`.
    - One libspu owner in `core/audio`. `native_moby_helpers` is split, and `releaseMoby` joins the allocator with one pool layout. The `g_TitlescreenState` fields and enum values go next to `kTitlescreenState`. `kCutsceneLayout` and `kInitNewGameState` are added.
    - `FieldScheduler` is deleted. `FrameDriver` holds `FieldOwner` plus a small skip-map/boot-window observer. The dead accessors and the `observeVblankCallback` chain are deleted.
    - `PressLatch` replaces `flybyPressHeld_`.
    - `pastBootPrefix` overrides replace both `bootComplete`s, so Spyro 1 now answers correctly during its logos.
    - `kGuestUpdateCycles`/`kGuestUpdateFields` replace the local 4-second budgets.
    - `BootPrefixRuntime` gets the Spyro 2/3 runtime bodies, and `spyro::Runtime::interpolates` returns true for all three. The empty Spyro 2/3 logo facts, `Runtime::title` and the duplicate titleName are deleted. `CameraMachineWords` is the one camera pair.
    - Spyro 2/3 warps read `kBootPrefixFacts.cut`, exposed as parseable constexpr so A5's tools can read them.
    - `hud_draw_context` moves into the Spyro 3 HUD.
    - Dead files deleted: `guest_execution`, `world_guest_execution`, test-only headers, `content_identity` with OpenSSL, `InputPhase::describe`.
    - Every Spyro narration block listed in the audit is cut to one line.
  - **Dead and forwarding code removed:** `dynarec_capabilities.h` (and Tomba's `port.cmake` probe for it), the unused executor/dispatcher/census accessors, `LightrecExecutor::execute`, the fallback default argument, `notifyExecutableStateReplaced`, `classifyGuestHostDispatch`, and `currentImageIdentity` (Core gains a const `imageCatalog()`; about 40 callers across c12, Tomba, TS2, Vagrant Story, Crash Bash and MMX4 are updated).
  - **Narration:** every listed cpu, core, hle, state, config, debug, input and frame/timing block is cut to one line.
- **Spyro proof:**
  - Spyro 1, 2 and 3 boot headless through logos to the title screen. Start still skips the screens the player does not control.
  - Spyro 1 warps to Artisans. Run 60 s of the recorded pad replay, then save state, load state and compare digests.
  - Pause menu quit-to-title still works (InitNewGameState).
  - Save to the memory card, restart, and reload the card.
  - `dumpram` and `SNAP_AT` write RAM plus scratchpad.
  - The debug server's `r`, `rw` and `cvars` answer.
  - aspect, ires and fps60 survive a settings round trip.
  - `PSXPORT_STORE_OBSERVE` prints hits.
  - Every other title smoke-boots to its title screen.
  - **Expected behaviour changes:** the display-pass guard now sees mirror writes; Spyro 1's picker panel stays hidden during its logos; crt0 refuses a field it cannot resolve. If the scan refuses on any title, stop and fix the scan.

### A4. Guest GPU decode, packet chains and emit memory
- **Owner modules and their files:**
  - `psx/present/{gp0_primitive_decode.*, host_ordering_table.cpp, emit_memory.*, emission_scope.*, gte_control.*}`.
  - `gpu_ports.*` and `ordering_table.*`, moved to `psx/gpu/`.
  - `psx/gpu/gp0_record_tap.cpp`.
  - `psx/platform/scea_splash.*` and `scea_asset.h`, which move to `psx-tomba/game/core/frame/`.
  - In other repos: Tomba's `boot_cards.cpp` and `asset.cpp`, Spider-Man's `frame_envelope.cpp`, Crash Bash's `game/render/packet_decode.*`, and Spyro's `game/core/title_logo.cpp` and `titles/spyro1/core/frame/logo_facts.h`.
- **Merged findings and their single implementation:**
  - `OtNode`/`OrderingTableCursor` own the node decode. `emitHostOrderingTable` is generalized into one packet-chain walker that takes a stride, head offset, single-chain start, bound and word reader. `kTagNextMask` is deleted.
  - `psx::present::DrawModeTracker` (keyed on `Gp0Op::SetDrawMode`, applied to everything except textured polygons) is the single E1 rule.
  - `applyTexPageAttribute` is rebuilt on the A2 TexPage decode.
  - `sext11` is deleted; `gp0_record_tap` calls `Gp0Command::signedAxis`, `toVram555` and `kVramWidth`.
  - `psx::present::HostMemory` gains merge-on-overlap, guest-seeded and zeroed maps, a LevelWord overlay, optional fall-through read recording, and `mapBytes`. `EmitMemory` gains first-read recording. `physical()` and `mainRamWord` now come from the memory map (A2), and `emitMemory` drops its private `physical()`. This becomes Spyro's memory model in B4.
  - `gte_control` uses `gte::kRotationWords` and `lerpWord`.
  - `gpu_ports` keeps the DMA2 walk and the GP1 tick composition. Its `gp0Write`/`gpuRead`/`loadImage`/`vram` forwards are deleted (mem.cpp was switched in A3) and so is the dead `sealRecord`, along with its doc lines. Titles call `core.gpuDevice()` directly.
  - The SCEA splash data and decode leave psxport for Tomba and use `decodeVramPixel`/`scale5to8`.
  - Spyro's `title_logo` keeps only its sprite-record parsing and bounds. It uses the psxport texel sampler, TexPage/CLUT decode, `scale5to8`, `VramPoint` and `kVramWidth`. Its `expand5`, `VramPoint`, texel fetch, `readPalette` and `texturePage*`/`palette*` are deleted.
- **Spyro proof:**
  - The Spyro 1 picker panel logo is shot before and after. Expect a visible fix: the 4bpp and 8bpp addressing was wrong.
  - In a Spyro 1 level, a record-path shot before and after should match with no regression.
  - Run `otattr` on a live frame.
  - Tomba's SCEA splash still shows.

### A5. Python tooling
Runs after A3, because `title_profile` reads A3's constexpr facts.
- **Owner modules and their files:**
  - psxport `tools/port/{consumer_verify.py, launch_environment.py, new launcher.py, looks_right.py, present_geometry.py, widescreen_pair.py}`, `tools/check_cpp_style.py`, `tools/formats/psx_exe.py`, `tools/decomp/images.py`, `tools/psxport_fetch.py`, `scripts/{bootstrap_workspace.py, submodule_state.py}`, `tools/automation/*`, `tools/oracle/console_capture.py`, `tools/prof_hot.py`, `tools/build.py`, and `tools/clean.py` and `tools/lint/` (both deleted).
  - All of `psx-spyro/tools/*` and the Spyro `CMakeLists.txt` cpp_ownership line.
  - Every other title's `tools/run.py`/`verify.py`/`bootstrap.py`, Spider-Man's `launcher_dependencies.py`, Vagrant Story's `runtime_boundary.py`, and Tekken 3's `probe_widescreen_pair.py`.
- **Merged findings and their single implementation:**
  - `consumer_verify.py` is the one configure/build/ctest/execution-boundary gate, with one consumer configure command and compiler resolution. Spyro and Vagrant Story become `ConsumerVerifyConfig` callers; Spyro's C++ quality check becomes a gate option.
  - `check_cpp_style.py` is the one format, line-cap and tidy check. Delete Spyro's `verify_cpp_quality` and `format.py`, and drop `--ownership-only` if nothing else needs it.
  - `launcher.py` owns the launcher: Host, distro family, the dependency-to-install table, require helpers, cache reset, configure and build. CPU count comes from `cpu_jobs`. Each title keeps only its own data.
  - `agent_environment`/`player_environment` own the run environment, including `PSXPORT_ASSET_DIR`. The hand-built environments in `shot.py`, `looks_right` and every title are deleted, as are `_LEGACY_HEADLESS_KEYS` and the `.spyro-toolchain` unlink. Spider-Man and Tomba replace `PSXPORT_NOWINDOW` with `PSXPORT_VK_HEADLESS`.
  - `provision_title.resolve_disc` is the one disc lookup; `drive.disc_path` and `shot.disc_path` are deleted.
  - `drive.launch_inputs` plus a title-aware `drive.environment` handle launch preconditions; `boot_run`, `title_route`, `live_play` and `shot` stop doing their own.
  - `psx_exe.py` gains a header reader; delete `images.py`'s copy and `re_globals.Image`.
  - Pillow is the one PNG/PPM codec. The hand-written codecs in `present_geometry`, `console_capture`, `shot` and `title_conversation` are deleted. (`str_decode.py` is deleted in B2.)
  - `psxport_fetch` `SUBMODULES`/`init_submodules` are the one submodule list, and `submodule_state.declared_entries` is the one `.gitmodules` reader.
  - Spyro's `title_states` owns the gamestate name table and `guest_globals.py` owns the guest words. The `drive.py` re-exports and aliases are deleted, and Spyro 2/3 addresses come from the C++ facts.
  - `ToolError` is the one refusal type once psxport is present, so the Refused-to-Refusal wrappers are deleted.
  - Dead code deleted: `go_public.py`, the unused automation helpers, the four `*control_settings.ini*` files, and the "pin" wording.
  - `tools/build.py` stays as psxport's one build entry point, rebuilt on `launcher.configure`/`build_targets`. Finding 252 asked to delete it in favour of `verify.py --no-test`, but `verify.py` went with the tests, so `build.py` is no longer a copy and the workspace build command names it.
  - `launcher.run` is the one process runner; `automation.process` keeps only `ToolError` and `cpu_jobs`.
  - `PSXPORT_DIR` still names a framework checkout in place of `external/psxport`, now in every title's `bootstrap.py` (finding 253 rewords its launcher line only).
  - `psxport_fetch.init_submodules` still moves every vendor to the commit psxport records on each launch and bootstrap.
  - Narration in the launcher and environment files is cut to one line per comment.
- **Spyro proof:** `tools/run.py` configures and builds (without exec), then `drive.py`, `shot.py` and `title_route.py` each run a headless Spyro 1, 2 and 3 boot to the title screen. The consumer gate passes for Spyro and one other title. Agents never run `run.sh`.

---

## Phase B (after the split / record-raster work lands)

### B1. Record raster, SDL render device and presentation ⚠ in-flight files
Flagged: frame_presenter\*, display_presenter\*, gpu_vk_present\*, gpu_vk_record\*, record_raster\*, record.frag, pane_composite\*.

- **Owner modules and their files:**
  - `psx/gpu/{gpu_device.*, record_raster_setup.*, record_raster_regions.h, texture_feedback.*, new record_geometry.*}`.
  - All 18 `gpu_vk_*` files, which move to `psx/render/*` as `psx::render::RenderDevice`, `RenderState` and `RenderWindow`. SDL_GPU stays the renderer.
  - `gpu_present_sink.h`.
  - `shaders_gpu/*`, `tools/gen_gpu_shaders.py`.
  - `present/{pane_composite.*, display_presenter.*, guest_widescreen_projection.*, picture_announce.*, present_plan.h, present_ledger.*, proj_params.*, render_mode.h, render_substrate.h, video_plan.h, viewport_layout.h}`, and `host_presentation.h` (moves to `host/`).
  - `frame/{frame_presenter.*, frame_dump_window.h (deleted)}`.
  - `host/panel_logo.cpp`, `ui/rmlui_render_gpu.*`, `ui/rmlui_overlay.*`, and `core/overlay_glue.*` (deleted).
  - In other repos, for the namespace move: c12 and MMX4 `presentationAspect` overrides, CTR, MMX4 and Tekken 3 `latchGuestProjection` callers, and the CTR and Vagrant Story geom readers.
- **Merged findings and their single implementation:**
  - **Names.** The SDL class becomes `RenderDevice` in `psx::render`, and `psx::gpu::GpuDevice` keeps the name. Stale guards become `#pragma once`. The global-namespace present types (`RenderPath`, `RenderMode`, `DisplayPassGuard`, `RenderSubstrate`, `ProjParams`, `VideoInputs`, the projection types and `PaneRect`) move into `psx::present`.
  - **VRAM.** `kVramWidth` replaces `kDeviceVram*`, `kRecordVram*`, the `kVramW` aliases and the 1024 wrap literal. `GpuDevice::picture` is the one VRAM-to-RGB24 decoder, so `writeVramImage` and its `<<3` decode are deleted. Shots and `PSXPORT_GPU_DUMP` then agree.
  - **Pipelines.** One fullscreen-pipeline builder on the device, taking shader, resource counts, format and blend, serves the device, `pane_composite::makePipeline` and `RecordRasterizer`. `makeBlitPipeline`/`m_blitPipe` are deleted, and `present.vert`/`image.vert` merge into `fullscreen.vert`.
  - **Samplers.** The device's nearest and linear samplers replace the `RecordRasterizer` and pane copies.
  - **Submit and readback.**
    - `submitAndWait` is the one bounded fence wait; the pane loop, `kFenceBudgetMs` and `OneShotWait` are deleted.
    - One device texture download plus one RGBA-to-RGB24 shot writer that counts non-black pixels serve `download_present_image`, `present_shot` and the pane shot.
    - One sampled-texture create plus upload helper serves `ensureImageTexture`, `ensure_targets`, `build_image`, `upload_device_vram`, `retainFilledPresentImage`, `makeTexture`, `PanelLogo` and the pane's white texture. Those last two submit through `GpuDevice::submit`.
    - `beginClearedPass`/`drawFullscreen` in the present sink serve the four fullscreen passes.
  - **Present sizing and display rect.**
    - `presentImageSize()` is the one present-size rule; `videoStatus`, `presentInputs` and `build_image_present_image` use it.
    - `DisplayPresenter::presentedRect(Core&)` serves `frame_presenter`, `buildDisplay` and `dumpDisplay`.
  - **Record path.**
    - `record_raster_setup.h` is the single op-layout source. `gen_gpu_shaders` writes `record_op.glsl` from it, and `record.frag` drops every mirrored constant.
    - `record.frag`'s dither LUT handles lines, so `kDitherTable`, `ditherChannel` and the CPU pack are deleted; `toVram555` is used elsewhere.
    - `record_geometry` owns the write-extent rules and `RecordRect`, and `UndrawnWrite` stores a `RecordRect`.
    - `FramePresenter` talks to Core directly; `FramePresentationBackend` and the injected commit are deleted.
  - **Overlay and forwarders.**
    - `overlay_glue` is deleted.
    - `buildDisplay`/`repaint` fold into `DisplayPresenter`, and `ensureDevice` folds into `PickerSession`.
    - An `RmlUiLibrary` RAII class owned by the device replaces the RmlUi globals, the `atexit` hook and `releaseDeviceResources`.
  - **Widescreen.**
    - One latch helper plus `nativeDisplaySize` serve both latch paths and `videoStatus`; `unlatchGuestProjection` is deleted.
    - One forward aspect mapping, `presentationAspectFor`. `classifyWide` and `Geometry` take `PresentationAspect`, and the inverse switch is deleted. The log keeps printing the setting's int so Spyro's `run.py` still parses it.
  - **Diagnostics in these files.** `DisplayPresenter` uses A3's `FrameSchedule` and typed shot lists. `dumpGuestRam`, `parseFrameAndPath`, `parseFrameList` and the GRAMDUMP knob are deleted. `dump_present` keeps its per-show hook on the shared schedule.
  - **Dead code.** The `ProjParams` camera and projection cluster, `guestCoordinatesWidened`, `samePicture`, `PRESENT_DISPLAY_ASPECT_H`, ledger accessors, `identityFor`, the `OtNode` accessors, `truncated_` and its narration, the `faulted()`/destructor/`int` latch, and `pixel_width`/`pixel_height` (replaced by `pixelSize()`).
  - **Narration** in every gpu, present, shader and pane block listed.
- **Internal resolution costs GPU only.** Today `PlanBuilder::addTriangle` walks scaled scanlines on the CPU and emits one quad per scaled row per plane, so CPU work and op count grow with ires. The CPU instead does a constant-cost setup per primitive at the target scale (edge equations, bounding box, the gpu.c coverage rule's integer inputs) and emits one op per primitive. `record.frag` evaluates coverage per pixel from those edges, with exact integer arithmetic (emulated 64-bit where needed), so there is one coverage rule and it matches gpu.c at 1x. Guest VRAM and the Beetle device stay native; ires changes only the host image size. Op counts at 1x and 4x must be equal for the same frame.
- **No dithering in the host picture (psx-spyro 0200).** `record.frag` stops dithering. The dither LUT, `kDitherTable`, `ditherChannel` and the CPU pack are deleted. The guest device keeps its dither bit as guest state.
- **Fades span the widescreen canvas (psx-spyro 0199).** Trace Spyro's fade primitive at 16:9. If the record rule for buffer-spanning draws is incomplete, fix it in `record_raster_setup`/`record_geometry`. If the guest site needs widening instead, write the owner into 0199 for B4.
- **Spyro proof:**
  - Op-count log lines for the same Spyro 1 frame at ires 1 and 4 are equal; shots at both scales show the same picture.
  - Spyro 1 sky gradient before/after shows no dither; a 16:9 shot mid-fade shows the margins darkened.
  - In a Spyro 1 level, take display-path and record-path shots at aspect 4:3 and 16:9, plus fps60 in-between dumps. The HUD should sit at the screen edges, logo fades should be intact, and there should be no cracks.
  - The picker composite shows all three panes and the pane shot reports coverage.
  - Run PSXPORT_GPU_DUMP and `shot` on the same frame. **Expected change:** the dump's 15-bit pixels get brighter, up to +7 per channel.
  - Check primitives that use dithered lines.
  - Operator does a real-time check.

### B2. CD, XA, audio and movie ⚠ native_fmv\*
- **Owner modules and their files:**
  - `psx/cd/*`, `psx/audio/*` (except `spu_device.cpp`), `psx/movie/*` (including `native_fmv*`), `platform/c_subsys.h` (deleted), `tools/fmv_export/*` (`str_decode.py` and its test deleted).
- **Merged findings and their single implementation:**
  - `cd_position` owns BCD and MSF/LBA conversion (signed lead-in), plus one `kLeadInSectors` and `toBcd`.
  - `CdController` owns Setmode/Setfilter state and `sectorRoute`. The XaStream drive copies are deleted, `applyCommand` writes the controller only, and the clip parameters stay with XaStream.
  - `XaStream::decodeAndStoreSector` is the one decode-to-ring path, with one history reset.
  - One Setmode framing function; `loadSector` reuses the raw read.
  - `routeDriveSector` is the one audio-vs-data routing step.
  - `CdcResponse` enum.
  - `Cd::StockResponse`; `publishStockSync`/`forgetStockResponse` are deleted, and one template entry replaces the eight trampolines.
  - `Hle::runAsInterrupt` and `GuestContextScope` are used in `cd_ready_delivery` and `stock_libcd`. The redundant `in_irq` check is dropped.
  - New `audio/xa_adpcm` owns the XA-ADPCM decoder and `kXaMaxSectorFrames`, with one coding-byte parse. A sector-header accessor set in `cd_position.h` serves the drive, `disc`, `native_fmv`, `fmv_export` and `discdump`; `repl_xadump` plays through a clip XaStream.
  - `fmv_decode` owns the STR demux (`videoChunk` plus `FrameAssembler`) and one macroblock tiler. `PSXPORT_FMV_ROWMAJOR` is deleted, and `fmv_export` reads `movie_max_frames()`.
  - `MdecDevice` owns MDEC (one run-to-park constant); `mdec_pump`, the forwarding wrappers and `c_subsys.h` are deleted. One `BeetleDeviceState` template serves SPU and MDEC.
  - `HostAudioOutput` owns the gate, claim and stream; Fmv uses it and `audio_policy.h` folds in.
  - `WavWriter` serves SPU capture, the REPL wav and `fmv_export`.
  - The rate constants: 44100 is `SpuFieldCadence::kSampleRateHz`, 150 sectors/s comes from `driveSectorsPerSecond`, and the XA default rate is named once. `MovieFieldGrid` uses `fieldIntervalMs` and `Timing::displayFieldRate`.
  - `summarizePcm` is the one PCM summary; the spudbg peak scan is deleted. The host clock helper replaces the hand-written timespec code.
  - The SPU bound core and debug counters become `SpuDevice` members. `PSXPORT_SPUBT` becomes a typed optional. The WAV `atexit` hook is replaced by RAII.
  - Dead code: `playLba`, `frameLogic`, the field-report machinery, `reprimes`/`drops`.
  - Narration and stale names in the cd, audio and movie files.
- **Spyro proof:**
  - Spyro 1 intro to title with XA music and sound effects (listen in real time).
  - Load a level through a warp.
  - Spyro 2 and 3 boot movies play, can be skipped, and stay in sync.
  - Capture a WAV and check its length and level.
  - Save and load state mid-XA.

### B3. Host shell, picker and pacing ⚠ psx/host/picker\*
Runs after B2.
- **Owner modules and their files:**
  - `psx/host/{picker_session, picker_runtime, picker_content, picker_layout, picker_composite, panel_sessions, product_host, saved_disc}.*`.
  - `psx/frame/{frame_pacer.*, pace_plan.h, frame_loop_shell.cpp, session_control.h}`.
  - `ui/{choice_navigator.* (deleted), choice_content.h (deleted), warp_control.cpp, dev_command_control.cpp, mod_row_model.*, menu_row.*, menu_document.cpp}`.
- **Merged findings and their single implementation:**
  - `FramePacer::pace()` is the one cadence path. `FramePacer::fieldRateMilliHz` is deleted (B2 removed its last outside callers), and `hostScreenPace` uses `this`. The picker uses `fieldIntervalMs` and the host-screen constant.
  - `PickerContent` owns the panel-to-title mapping. `PanelSessions` owns the selection plus one `step(dir)` built on a shared `ui::wrap`, which `WarpControl` and `DevCommandControl` also use. `PickerRuntime` and `PickerLayout` stop keeping their own copies of the selection.
  - The picker uses `host_input.h` buttons. **This fixes Left/Right**, which were wired to the L2/R2 bits.
  - `panelReady` is the one visibility test.
  - `splitSlugs` uses `splitList`.
  - Dead code deleted: `ChoiceNavigator`/`ChoiceContent`/`content()`/`heading`, the Adjust-row machinery, the `pace_plan`/`session_control` namespace-before-pragma, and the `c_subsys.h` include.
  - `frame_loop_shell` calls `frameDriverAs`.
  - Narration trimmed.
- **The selector is a screen of the product (psx-spyro 0197).** It is presented through the same frame present path, overlay, RmlUi menu and input owners as a game frame, with the panels as that screen's content. The separate picker composite path is deleted. Panel pictures come from each panel session's own image at its own resolution, and nothing is cover-cropped or magnified.
- **Spyro proof:**
  - The in-game RmlUi menu opens over the selector and its settings apply there.
  - Picker cycles Spyro 1, 2 and 3 with D-pad Left/Right (**changed**: L2/R2 no longer move it) and through the control channel's `select left|right`.
  - Each panel boots and the selection follows.
  - Menu rows and the dev panel work.
  - Real-time pacing has no stutter in the picker or in game.

### B4. Spyro render
Runs after B1 (the present namespace) and A4.
- **Owner modules and their files:**
  - `psx-spyro/game/render/*`, `titles/spyro1/render/*`, `titles/spyro2|3/render/*`.
  - `titles/spyro1/core/hud/{screen_overlays, hud_anchor}` and `core/frame/ordering_table`, which move to `titles/spyro1/render/`.
  - `native_glow_sparkle_pools`, which moves to `render/`.
- **Merged findings and their single implementation:**
  - **Memory and packet chains.** psxport's `EmitMemory`/`HostMemory` replace `spyro::guest_memory::WorkingMemory`/`GuestMemory`/`HostMemory`/`canonicalAddress`. `PacketArena` and `SavedDraw` stay. The psxport chain walker plus `DrawModeTracker` replace `PacketChains`/`DrawMode` and the four decode/abort/emit loops. `Rebuild::forEachPacket` and `assignPair` use psxport link decoding.
  - **In-between helpers.** One `actors::InBetween` serves the six producers with a single overlay order (check which order is right). `saveDraw` sits beside `packDraw`. One spyro-level producer and override registrar serves the 11 registration pairs.
  - **Working ranges and record bytes.** `binnedWorkingRanges` and `mapBinnedWorkspace` (with a split-out part) own the working ranges. `Core::readBytes` and `HostMemory::mapBytes` replace the record loops and `actors::readBytes`/`mapBytes`.
  - **Terrain.**
    - One GT4-piece and one GT3-piece emitter in `guest_terrain_split` serve the Spyro 1 and engine coarse and fine passes, with `polygonBin` taking a bias.
    - One crack walker; one `kCrackEntrySize`.
    - Horizontal sphere margins in `guest_moby_frustum`.
  - **GP0 and link helpers.** GP0 vocabulary comes from psxport `gp0_command` (A2); the file-local tags and codes and the five `kGt4ToGt3` copies are deleted. `linkAfter`/`linkIntoBin` replace nine tag writes and the bin-insert copies.
  - **Arithmetic helpers.** In `guest_word.h`: `signedHalf`, `asWord`, and `lerpWord` (from psxport). `quotient` replaces `divLo` and the paired-actor quotient. A short-way lerp template, plus width-generic load/store, serves angle and particle coordinate lerps.
  - **GTE and HUD.**
    - `Gte::loadRotation` replaces the 21 rotation loops; `guest_moby_rotation::load` is deleted.
    - One `guest_magnitude::magnitude`.
    - `cameraSineTable` plus `kCosineOffset`; `Facts::sineTable` and `cameraCosineTable` are deleted.
    - One `ui_anchor::correction` reads the plan margin. Deleted: Frame, widescreenFrame, the refusals, the per-title `frame()`/`columns`/`shiftArgument`, and a shared `shiftRegister`.
    - One Spyro 3 drawer template.
  - **Spyro 1 render facts.** Literals come from A3's `guest_globals` (sine/cos, light struct, models, camera, scratchpad + offset, `kRenderGlobals` packet cursor/OT); `ScreenOverlays::kPacketCursor` and `OrderingTable::kBinTablePointer` are deleted. Glow and sparkle pool layout is owned by `glows.h`. Header helpers used in only one translation unit become anonymous-namespace functions.
  - **Narration** in the HUD and render blocks.
- **Gameplay in-betweens judder (psx-spyro 0196).** Reproduce with fps60 on `warp 10` per title. Find the object whose in-between position does not lie between its two records, or the camera and an object blended on different terms, and fix it at the producer or pairing that places it. Follow the interpolation contract: identity from a draw-site tag, transforms lerped and never screen vertices, and a frame with any unowned entry shown as frame n whole.
- **Spyro proof:**
  - In Spyro 1, 2 and 3 levels (warp), take fps60 in-between dumps and record-path shots before and after. Terrain, actors, sky, glows, sparkles, particles, shadows, sprite queue, HUD anchors at 16:9, and the letterbox/tint must match.
  - **Expected change:** the unified E1 rule now applies texpage and blend to textured sprites.
  - Operator does a real-time 60 fps check on all three.

---

## Order

A1 → A2 → A3 → A4, with A5 any time after A3. Then, once the in-flight work lands: B1 → B2 → B3 → B4.

## Files edited by two groups
Each is a call-site or knob edit forced by an API change that cannot be split.

| File | First group | Second group's edit |
|---|---|---|
| `psx/config/{diagnostic_config.*, config_vars.h, config.cpp}` | A3 | B1 deletes GRAMDUMP, types the shot lists and deletes `user_state_dir` once their consumers move. B2 deletes ROWMAJOR and types SPUBT. |
| `psx/core/core.h` | A3 | B1: the `rsub` member type is qualified as `psx::present::RenderSubstrate`. |
| `psx/game/game.h` | A3 (`frameDriverAs`) | B1: the SDL device forward declaration and member type. |
| `psx/host/title_session.cpp` | A3 (host turn, store observer, `pad.init`) | B2: the audio claim moves to `HostAudioOutput`. |
| `psx/cd/stock_libcd.cpp` | A3 (`serviceCdc`, 2 lines) | B2: everything else. |
| `psx/debug/debug_commands.cpp` (REPL `shot` line) | A3 | B1: `presentedHeight` becomes `presentedRect`. |
| `psx/movie/native_fmv.cpp` | B1 (render include and name) | B2: everything else. |
| `psx/host/picker_session.cpp` | B1 (`ensureDevice` fold) | B3: everything else. |
| `psx/host/title_session.{h,cpp}` | A3, B1, B2 | B3: the pane host hooks, the picture accessors and the deleted content-size helpers (0197). |
| `psx/movie/native_fmv.cpp` | B1, B2 | B3: `fieldRateMilliHz` read from `Timing::displayFieldRate` once `FramePacer::fieldRateMilliHz` is deleted. |
| `psx/present/display_presenter.{h,cpp}` | B1 | B3: `buildPanes`, the selector's present (0197). |
| `psx/render/{render_device.*, render_state.h, render_present.cpp, render_readback.cpp}` | B1 | B3: the pane pipeline and white texture; the content measure and `PresentedImage::content` deleted (0197). |
| `psx/game/mods.{h,cpp}` | A3 | B3: `Mods::reload`, so a menu setting reaches the running panel. |
| `psx/host/panel_logo.*` | B1 | B3: the logo magnification deleted. |
| `ui/menu_pane.h`, `ui/menu_tab_bar.cpp` | none (outside B3's list) | B3: the dead row-builder comment; tab stepping on `ui::wrap`. |
| `psx-toy-story-2` frame boundary | B2 | B3: the `fieldRateMilliHz` caller, as `native_fmv.cpp`. |
| Spyro render files that include `core/guest_globals.h` (if any) | A3 (include path) | B4: everything else. |
| `psx-spyro/titles/spyro1/core/frame/logo_facts.h` | A3 | A4: the logo record loses `clipWidth`. |

## Shipped behaviour that changes on purpose
State each one in its group's commit and its issue doc.

- Debug shots get the correct 5-to-8 expansion.
- The Spyro panel logo gets correct 4bpp and 8bpp addressing.
- One E1 draw-mode rule applies to every title.
- The display-pass guard now catches mirror writes.
- `deliverEvent` runs on the exception stack.
- PAL titles get a PAL field budget.
- crt0 refuses a field it cannot resolve.
- Spyro 1 reports that it is still in its boot prefix while the logos play.
- The picker moves with D-pad Left/Right.
- B3 (psx-spyro 0197): `picker shot` is removed (`pshot` captures the selector); the selected panel shows its live presents, fades included; a paused panel is drawn 1:1, centred and cut by the seams, so it shows a centre slice of a wider picture; each panel shows the title's own black border rows as bands at its top and bottom, since nothing is cropped; the fps60 menu row is hidden on the selector; `select left|right` replies `ok: step left|right` instead of `ok: panel N (slug)`.
- A5, every title's launcher: the player build moves to `build/player` and psxport's disc reader to `build/player-tools`; the launcher takes only an optional positional disc and `--prepare-only`. Dropped: CTR's `--headless` (agents use `agent_environment`), Crash Bash's and C-12's `--check`, and C-12's `--disc` (now the positional disc).
- A5, Spyro's launcher no longer requires OpenSSL; nothing in Spyro links it since A3 deleted `content_identity`.
- B1, the GPU-fault marker moves from the per-repo user state dir to Lucent's user data dir (`$XDG_CONFIG_HOME/psxport/gpu_fault` on Linux), so one fault refuses the GPU for every title on the machine.
- B1, the image pass always renders in the present-image format; the swapchain and pane sink draw through a separate `sinkPipeline` in the swapchain format, where before one pipeline took the swapchain format whenever a window was up.
- B1, at ires above 1 the record planner splits batches by gpu.c's 1x coverage; an op that overlaps an earlier op of its batch only inside one native pixel blends against the batch's snapshot there.
- B2 (issue 0165): a register-path Setmode clearing STRSND stops XA; Mega Man X4's Setfilter reaches the drive; an open-ended pull stream routes by the drive's 0x64 rule; a lead-in Setloc sets no position; the register FIFO honours Setmode 0x10 (2328 bytes from offset 24); the XA ring drops once less than 4032 frames are free; movies respect the audio claim; `Fmv::begin` powers the MDEC instead of `title_session`; PCM summaries only with `audiofield` on; `fmv_export` writes the MDEC's 24-bit frames; the CD ready callback runs under `InterruptScope` with the sentinel `ra` on the exception stack.
- B2 (issue 0165): a state save or load opens the configured memory card when the guest has not; `xadump` plays file 1 of the channel through a clip `XaStream` at 44.1 kHz and skips other channels' EOFs.
- B4 (psx-spyro 0196), every title with StateProducers (Spyro 1/2/3, Crash Bash, Tomba, Toy Story 2): an fps60 in-between whose record holds any primitive no producer owns, or a render that cannot be placed, shows frame n whole instead of mixing lerped and unlerped primitives. Today every gameplay record in those titles holds unowned primitives, so gameplay fps60 shows 30 fps motion until the remaining producers exist; Spyro 1's actors, sky, glows and other owned objects no longer move on in-betweens. Crash Bash, Tomba and Toy Story 2 were only boot-checked for this change.
- B4: a CLUT the shown frame never sampled is read from the device VRAM for a render, where before that object kept frame n's entries.
- B4: Spyro's producers carry the E1 draw mode through psxport's `DrawModeTracker`, so a textured sprite in a produced chain takes the texpage and blend; no Spyro emitter writes a textured sprite today, so no picture changes.

## Performance (applies to every group)

USER, 2026-10-20: "even at 4k/60 it should run fine on weak hardware" / "don't test against wall clock CPU speed but just make sure the code is optimal".

Each group leaves the code it touches optimal. That is judged by the work the code does, not by timing on this machine:
- No CPU work grows with internal resolution. Per-pixel and per-scaled-row work goes to the GPU (B1 owns the record raster change).
- Per-frame work is done once, with no redundant passes, copies or conversions.
- No allocation in the steady-state frame.
- No GPU readback in the frame path. No avoidable full-VRAM pass.
- Work is proportional to what the guest drew, not to the screen size.

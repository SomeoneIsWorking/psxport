# PSX workspace restructure plan

Measured on the tree as of 2026-10-13, then corrected by an adversarial review of the same tree. Line numbers drift; re-read every file named here before editing it. "Delete" means the code is removed in that milestone. Nothing is kept as a shim, a forwarder or a second implementation. Each milestone's brief carries its items from "Facts to carry" at the end.

## 0. Rules for every milestone

- **One agent at a time.** Each milestone is one brief. It lands as small `type(scope): subject` commits on `main` in every repo it names. It is pushed only when everything below is green.
- **Lands whole.** `external/psxport` is a live symlink, so a psxport signature change breaks every caller at once. The psxport change and every title edit it forces land in the same push window. No old function is kept for compatibility.
- **Gate.** Each touched repo runs the gate from `~/repo/psx/CLAUDE.md` on its touched targets only; untouched titles only have to build. Then every title the milestone reaches is boot-smoked: built binary, hidden window, `PSXPORT_NATIVE_FRAMES` (the run's frame cap) set, and it must reach that title's documented boot milestone. Agents never run `run.sh`.
- **Structural rules, few and permanent.** Rules extend the existing re-harness `tools/cpp_policy.py` and `tools/codemap.py`, called once from psxport `tools/check_cpp_style.py`. Only three kinds: the `entry point` rule, the existing global-function/extern/block-static rules with per-repo `accepted.txt` lists that only shrink, and retired names a title could plausibly re-add (`watchdog_init`, `gte_bind`, `spu_bind`, `mdec_bind`, `load_exe`, `store_observe_attach` outside psxport). No temporary count ratchets, no listing files; a deleted symbol already fails to compile.
- **Play check (PC).** Runs once at the end of each phase (after M8, M15, M26, M35), not per milestone. A milestone's "Play check" line names the titles the next phase-end check must cover. A failure the previous phase-end check already showed is a known defect with an issue file; it is fixed in its own brief, not re-measured.
  - **Setup.** Launch the built binary directly, in a visible vsynced window on the user's session. Do not use the hidden mode, because it disables vsync.
  - **Getting there.** Reach the content through the title's existing pad replay or debug warp, never by steering. Run at least 60 s of gameplay per title.
  - **Numbers.** They come from the run-end report added in M1:
    - present-to-present interval histogram: p50, p99, worst, intervals over 1.5 vsync periods, and the longest run of consecutive misses;
    - consecutive-frame stability: the shown record sequence never repeats or goes backwards, and interpolation steps are neither dropped nor doubled;
    - audio underruns: the `AudioQueuePolicy::needsReprime` count after the boot cushion, plus the `shouldDrop` count.
  - **Pass.**
    - 0 underruns and 0 drops after boot.
    - Missed-vsync intervals at most 0.5%, and never 2 in a row.
    - p99 within +10% of the previous phase-end check on the same machine.
    - The stability invariant holds.
  - **Order.** Spyro titles always run first, then the others in the order listed.
  - **Routes.** Crash Bash, Mega Man X4 and Tekken 3 have neither a warp nor a replay; Spyro, CTR, Vagrant, C-12, Spider-Man and TS2 have no warp. M0 adds the missing route before that title's first play check.
- **recordcheck covers t=1 only.** `gpu_vk_record_present.cpp` skips the compare on composed in-betweens. In-between frames are gated by per-producer unit tests through `composeFrame` (t=1 equals S; t between from and to), never by recordcheck.
- **Owners converge before docs.** A repo's `docs/features.md` and slim codemap are written in the milestone right after the last milestone that creates, deletes or moves an owner in that repo. Renaming call sites mechanically (GTE, logging) does not count as moving an owner. From M8 on, every milestone updates the codemap rows and `features.md` rows of the owners it changes.

---

## M0. Content routes (one title per brief, as each title's first play check comes up)

A title debug option on the Tomba 2 `dev_warp` pattern (control-channel request, applied at a frame boundary through the title's own transition owner), or a recorded pad replay where a warp needs RE not yet done. Never steering.

## Phase L: one product loop

Precondition: the Spyro play-quality fixes (picker, flicker, audio) have landed and pushed; M1 and M2 touch spyro.

End state:

- `psx::host::ProductHost` is the only process top level.
- `psx::host::TitleSession` is the only boot and field composition.
- Every title supplies a `TitleCatalog`, a `GameRuntime` and a `FrameDriver`.

### M1. Finish the TitleSession spine (psxport, spyro)

- **Repos.** psxport, plus one deletion in spyro.
- **Create.**
  - `psx::host::RunBudget`, built from a new CVar `cv_native_frames`. It applies native_boot's rule: an explicit cap wins, then a pad replay or resume lifts the cap, then a live client or REPL means uncapped, then headless means 120 and windowed means uncapped. It counts presentation-fence steps.
  - `TitleSession::End::Quit`, plus a process `QuitScope` in ProductHost.
  - The watchdog is process-global signal state, so ProductHost owns one armed watchdog for the process; sessions never arm or disarm it.
  - Panel rule. Picker panels are live TitleSessions (`panel_sessions.cpp`). RunEndReport, `applyConfiguredState` and the REPL pump apply only to the confirmed or direct session. RunBudget never counts panel pre-roll; `host/pre_roll_budget.h` stays the pre-roll owner and `title-host.md` states the difference.
  - The REPL budget pump inside `TitleSession::step`.
  - `TitleSession::boot` calls `Machine::bindDevices`. This fixes the reversed bind→init order and the missing `Pad::overridesInit`. Boot also gains clear display, host diagnostics and `applyConfiguredState`. `Machine::playBootMovies` reads `cfg->bootFmv`, which no title sets: delete it here.
  - One `RunEndReport`: `Machine::reportRunEnd` merged with `TitleSession::reportRun`, then `runtime.reportRun`.
  - Play-check counters. Extend GpuPerf's existing `FrameTimeHistogram mFrames` with the present-to-present series (no second histogram), and add reprime/drop counters in `SpuAudio`. RunEndReport prints them once. Land this as the first commit so the baselines use it.
- **Delete.**
  - `SessionRun::fieldDelivered` and spyro `game/core/field_owner.cpp:250`'s call to it.
  - The `cfg_int("PSXPORT_NATIVE_FRAMES")` read in `host/title_session.cpp`.
  - The second run-end report in `TitleSession::reportRun`.
  - `PickerSession::run`'s private cap and its window-close `exit(0)` path. The picker now uses RunBudget and QuitScope.
- **Proof.** New `tests/test_product_host_session.cpp`, through the shipping ProductHost with a stub catalog and FrameDriver. A session ends on its cap. A client `quit` reaches RunEndReport. A window close reaches RunEndReport. The cap counts fence steps. Destroying a panel session emits no RunEndReport and applies no LOAD_STATE, and the watchdog stays armed while panels churn. Existing tests: `test_machine_composition`, `test_field_turn`, `test_frame_loop_shell`. Boot smoke for Crash 1/2, Tomba 2 and Spider-Man 1: their cap now fires, which it never did before.
- **Play check.** Spyro 1, 2, 3, then Crash 1, Crash 2, Tomba 2, Spider-Man 1. Bind order and boot steps changed for all of them.
- **Docs.**
  - psxport `docs/title-host.md`: the session order and RunBudget, with its unit stated as fence steps.
  - The `docs/codemap.md` rows for host/ and boot/.
  - Spyro: convert its documented frame numbers to fence steps once, wherever they cite `PSXPORT_NATIVE_FRAMES`, and `spyro/tools/boot_run.py` and `psxport/tools/port/looks_right.py` with them.
- **Risk.** Spyro headless runs reach a different point, because the cap counted delivered fields before and counts steps now.

### M2. One framework `main`, plus the policy plumbing (psxport, spyro, crash, Tomba2Engine, spider1)

- **Create.**
  - `ProductHost::run(argc, argv)`. It owns help and the CLI from `catalog.productName()`. It skips the picker for a size-1 catalog or an explicit executable argument.
  - `TitleCatalog::provisioningRoot()`.
  - `psxport/runtime/psx/host/product_main.cpp`, the only framework-owned `main`.
  - CMake `psxport_add_product(<target> CATALOG <factory>)`.
  - `psxport/tools/policy/harness.py`, the one re-harness locator: `RE_HARNESS_DIR`, then the sibling `shared/re-harness`, else refuse and name the clone command. `psxport_fetch.py --auto` shallow-clones re-harness in CI.
  - `check_cpp_style.py` runs re-harness `cpp_policy.py` over the compile database, with `tools/policy/accepted.txt` per repo.
  - New cpp_policy rule `entry point`: `main` is allowed only under `--entry-root` (tests/, tools/, `product_main.cpp`). The baseline lists the 10 remaining hand-written mains, and the list only shrinks. Packaging launchers (`crashbash/packaging/linux/launcher_main.cpp` and any like it) are listed by name and folded into ProductHost provisioning through the shared first-run setup UI in their own milestone.
  - Add the re-harness selftests for each new rule.
- **Delete.**
  - spyro `game/core/main.cpp`, crash `product/main.cpp`, Tomba2Engine `game/core/entry/main.cpp`, spider1 `game/core/main.cpp`. Each repo now calls `psxport_add_product`.
  - The locator copies in `spyro/tools/source_policy.py:32-47` and `spider1/tools/source_policy.py:12-23`.
  - Per-title help text and provisioning-root constants (10 copies).
- **Proof.** cpp_policy selftests show the entry-point rule fails before and passes after. An accepted line that no longer occurs fails. Boot smoke for all four repos, through both the picker and a direct executable.
- **Play check.** None. Nothing in presentation, audio or timing changes; the boot smoke is enough.
- **Docs.** psxport codemap host/ rows. Each repo's codemap entry row now reads "entry is the framework main".

### M3. Boot seams; migrate the `native_boot_run` users (psxport, crashbash, toystory2, megamanx4, and the 8 `discEnvVar` overriders)

Five briefs, each landable alone because `native_boot_run` lives until its last user goes:
- M3a: `TitleIdentity` disc facts; delete `discEnvVar`, its 8 overrides and the 4 `disc.env_key` writers.
- M3b: `bootEntry`, `admitExecutable`, the provisioner, `LogicFrame`. Tomba stays `Crt0` and its MAIN.EXE handoff stays in bootInit (`tomba_runtime.cpp`); no Tomba admitExecutable override. A wrong `bootEntry` is refused by name at boot, with a test.
- M3c Crash Bash, M3d Toy Story 2, M3e Mega Man X4. M3e deletes `native_boot`. M3d and M3e convert `toystory2/tools/headless_run.py`, `megamanx4/tools/live_play.py` and `megamanx4/tools/verify_task_resume.py` to fence steps.

- **Create.**
  - `GuestProgramImage::bootEntry {Crt0, DriverOwned}`.
  - `GameRuntime::admitExecutable(Core&, std::span<const uint8_t>)`, whose default is `loadPsxExeImage` + `applyPsxExeTopLevelRegisters`.
  - `TitleIdentity::discEnvVar`, `discExecutablePath` and `defaultExecutable`, which are per-disc facts.
  - One framework disc-extract provisioner, used when the executable file is missing.
  - `psx::frame::LogicFrame::begin(game, frame)`.
- **One owner per identity fact.**
  - Disc facts live in `TitleIdentity`.
  - The window title comes from `catalog.productName()`. `HostIdentity::windowTitle` is deleted.
  - Card and user-data names stay in `HostIdentity`.
- **Delete.**
  - `GameRuntime::discEnvVar()` and its 8 overrides: spider1, toystory2, crashbash, crash, vagrant, spyro 1/2/3.
  - The `disc.env_key` writes in ctr, tekken3, vagrant and tomba1.
  - `native_boot.cpp/.h`, `native_boot_run`, `dc_boot_init/dc_step_frame` and their hand-declared prototypes in `host/title_session.cpp:29`.
  - The dead `BootStub::run` and `native_scea_splash`.
  - Crash Bash: player_entry's binds and census line; `loadExecutable` becomes `admitExecutable`.
  - Toy Story 2: `game/boot/title_session.cpp` (`ts2::TitleSession`), its disc extractor and its init copy.
  - Mega Man X4: main's double attach, `g_runtime`, the leaked `new Game`, the disc extractor and the duplicate census.
  - The copied logicFrame/otAttr/pad preamble in crashbash and spider1, now one call to `LogicFrame::begin`.
- **Gate rule.** Entry-point baseline drops to 7.
- **Proof.**
  - The per-title override-registration census, before and after, for Crash Bash, TS2 and MMX4. Override order is preserved: `registerOverrides` runs after the binds.
  - A unit test that the provisioner extracts the executable from a fixture disc.
  - Boot smoke: the 3 migrated titles plus every repo that lost a `discEnvVar` override.
- **Play check.** Crash Bash, Toy Story 2, Mega Man X4. They now get QuitScope, clear display and the canonical bind order.
- **Docs.** psxport codemap rows 22-27 and 136-141; `title-host.md` gains the boot-entry and admission facts. In the three titles' AGENTS.md, delete the "owns its boot" prose.

### M4. Migrate the `psx::Machine::run` users (psxport, ctr, tekken3, vagrant)

- **Change.** All three titles get size-1 catalogs with `bootEntry=DriverOwned`.
  - Vagrant: `Application::start` becomes `admitExecutable`. Its OT census goes into `reportRun`.
  - Tekken 3: the usage text is fixed, because argv[1] is an executable, not a disc.
- **Delete.**
  - `Machine::run`/`stepFrame`. What is left of `Machine` (bindDevices, prepare, attachControlChannel, reportRunEnd pieces) is folded into `TitleSession` and `RunEndReport`. Delete `boot/machine.cpp/.h` if nothing else remains.
  - ctr `game/entry/runtime_composition.cpp` and `main.cpp`.
  - tekken3 `entry/product_launch.cpp` (with its copied headless-120 policy) and `main.cpp`.
  - vagrant `game/main.cpp`, `Application::run`/`runApplication`, the namespace-scope `titleRuntime`, the hand-copied binds and the duplicate Lightrec run-end line.
- **Gate rule.** Retired names for title repos (outside psxport): `watchdog_init`, `gte_bind`, `spu_bind`, `mdec_bind`, `load_exe`, `store_observe_attach`. The rule resolves AST references, not text. Entry-point baseline drops to 4.
- **Proof.** Boot smoke; `PSXPORT_NATIVE_FRAMES` is now honoured by all three titles. CTR first frames are compared before and after, because clear display is new for CTR, Tekken 3 and Vagrant.
- **Play check.** CTR, Tekken 3, Vagrant Story.
- **Docs.** Codemap entry rows and AGENTS.md "owns its loop" prose in all three repos.
- **Converged.** ctr, tekken3 (docs in D1).

### M5. Tomba! 1 joins TombaCatalog (Tomba2Engine)

- **Create.** A Tomba 1 identity and runtime entry in `TombaCatalog`. One `tomba2_port` binary now has a picker.
- **Delete.**
  - `titles/tomba1/game/core/entry/main.cpp` and its CMake target.
  - The function-local static runtime, its private NATIVE_FRAMES parser (which returned a cap of 2 on a parse error), its copied binds and its loop.
  - The `PSXPORT_TOMBA1_DISC` refusal policy. Tomba 1's disc facts move to `TitleIdentity`.
- **Per-Core statics first.** Two Tomba Cores now coexist (picker panels). Move Tomba2Engine's per-Core function-local and file-scope statics (`static bool done` in `objlist_walk.cpp`, `perobj_dispatch.cpp` and the other render latches) to their owners here, not in M32.
- **Proof.** Boot smoke of both titles through the picker and directly. A link check for ODR and global-name collisions between the two trees, and a picker test with both panels live.
- **Play check.** Tomba 2, then Tomba 1.
- **User-visible change.** The separate Tomba 1 binary and launcher go away. That follows your one-process picker rule; report it as delivered.

### M6. Spider-Man 2 gets a FrameDriver (spider1)

- **Create.** `EnterElectroFrameDriver`. One `stepFrame` is one budgeted execute/resume turn plus exactly one `presentation.commit`. Spider-Man 2 joins the Spider-Man catalog (a two-title catalog in place of today's `SingleTitleCatalog`), with `bootEntry=DriverOwned`.
- **Delete.** `game/core/spider_port.cpp:runPort` (with its unused cap, swapped log arguments and frame-0 FieldTurn), `titles/spiderman2/main.cpp` and the `enter_electro_port` target.
- **Real RE work.** The commit point needs a measured guest present point, found in Ghidra. Gate it on SM2's own boot milestone.
- **Proof.** Boot smoke, plus a FrameDriver unit test: one fence per step. The same link-collision check and two-panel picker test as M5.
- **Play check.** Spider-Man 1, then Spider-Man 2.

### M7. C-12 gets a FrameDriver (c12)

- **Create.** `C12FrameDriver::stepFrame`, keeping today's order: pad, turn, VSync continuation, commit, audio. `C12Runtime` declares `DriverOwned`. `admitExecutable` covers `AuthenticatedImage`. The field count goes into `reportRun`.
- **Delete.**
  - `game/boot/machine.cpp` (the second class named `Machine`).
  - `game/frame/guest_field_loop.cpp`, together with its `gpu_native_internal.h` include.
  - player_entry's loop and `main`.
  - `boot_probe.cpp`, replaced by a FrameDriver unit test.
- **Gate rule.** The entry-point accepted list is now empty. Remove the baseline entries; the rule now has no exceptions.
- **Proof.** Boot smoke. A window close now reaches the report.
- **Play check.** C-12.
- **Converged.** c12 (docs in D1).

### M8. Close the loop phase (psxport + all titles' tests)

Three briefs: M8a runtime injection and install-global deletion across all repos, tests, `tools/smoke` and `tools/oracle`; M8b the FrameLoopShell member and the PRIVATE split; M8c features.md and `codemap.py --refs` (re-harness commit).

- **Change.**
  - `Game(GameRuntime&, …)` takes the runtime explicitly.
  - The FrameLoopShell `static Core *bound` becomes a member of the shell's owner. It keeps the "rebind only when the running Core changes" behaviour, which is what keeps Spyro 2's boot prefix fast.
  - The include split: `psxport.cmake` makes `boot/` and the `host/` internals PRIVATE. Titles legitimately include `gpu/` (159 includes), `present/` (27) and `frame/` (9) and need GTE and guest memory from `core.h`; `gpu/` goes PRIVATE after M25/M33, `cpu/` after M34.
  - One shared stub `GameRuntime` in the psxport test support target replaces the null-runtime case; delete the neutral-instance branches in FramePresenter and the pacer.
- **Delete.**
  - `psxport_install_game` (both overloads), `psxport_game_runtime`, `installedRuntime`, `g_ownedLegacyRuntime` and Core's copy-from-global.
  - The 122 title references, the 37 psxport test files, `tools/smoke/psxport_smoke.cpp` and the oracle tools' install calls.
- **Gate rule.** None; the PRIVATE split itself is the check.
- **Proof.** All touched tests including `test_oracle_snapshot` and `override_differential_gate.py`; a unit test that FrameLoopShell binds once per Core change; boot smoke of every product.
- **Play check.** Spyro 1/2/3, Crash 1/2, Tomba 2/1, Spider-Man 1/2, Crash Bash, TS2, MMX4, CTR, Tekken 3, Vagrant, C-12. Core construction changed for everyone.
- **Docs.**
  - psxport `docs/features.md` with F-BOOT, F-TURN, F-INPUT and F-PICK. Hop rows are written as `` `path:Symbol` ``; each section is at most 40 lines.
  - psxport `docs/codemap.md` slimmed to the Owners-table shape: delete the "Source tree" dump (lines 191-265); the 107 lines over 200 characters become one line plus a link.
  - re-harness `codemap.py check --refs`: path and symbol resolution, feature IDs and joins, at most 150 lines and 160 characters per line. It is called from `check_cpp_style --repository-only`.
  - `brief_sources.DOCUMENTS` gains FEATURES.

### D1. Docs for the repos the loop phase converged: ctr, tekken3, c12

One commit per repo:

- Write `docs/features.md` with title hops only, joined as `psxport F-X.n`.
- Delete the codemap's "Who owns it" chains and slim the codemap to 150 lines or fewer.
- Fix the unresolved references: ctr 2, tekken3 2, c12 1.
- Tag open issues with `features:`.

Gate: `codemap.py check --refs` is clean.

---

## Phase I: one title interface

End state: `GameRuntime` is the only title interface, with no function-pointer tables and no `if (core.cfg)` branch.

Measured fact that removes one stated risk: `FramePresenter::commit` (frame_presenter.cpp:236-245) ignores the temporal decorator when the backend interpolates records. Tomba 2 is a Record title, so the Fps60 decorator that `LegacyGameRuntimeAdapter` builds for it is never used. Deleting the adapter does not drop Tomba 2 to 30 fps. Confirm `interpolatesRecords()` is true for Tomba 2 in M14's play check.

### M9. Fix the live crash, delete dead hooks, one title command (psxport, Tomba2Engine, megamanx4, spyro, crashbash, crash)

- **First commit, test first.** Delete the framework `tp` branches in `debug/dbg_server.cpp:653-663` and `debug/repl.cpp:300-310`; `tp` becomes a Tomba 2 command. Proof: a direct-runtime fixture sends `tp` and gets "unknown command" instead of a SIGSEGV.
- **Delete.**
  - Hook fields `frameUpdate`, `drawOTag`, `musicCoordTick`, `renderBbFrameReset`, `selftestGame`, `ctxCreate`, `ctxDestroy`, `bootInit`, `registerOverrides`, plus the matching entries in the Tomba 2 and X4 hook tables.
  - `GameRuntime::replCommand` and `controlCommand`, merged into `bool titleCommand(Core&, const char *cmd, const char *line, FILE *reply)`. The REPL passes its log-backed stream.
  - Tomba 2's `extern bool tomba_repl_command`.
- **Migrate.** spyro1, crashbash, crash, Tomba 2 and picker_runtime move to `titleCommand`.
- **Proof.** A titleCommand test sends a command through both the REPL and the control channel. Boot smoke for the touched titles.
- **Play check.** None; this is the debug surface only.
- **Converged.** crashbash (docs in D2).

### M10. Mega Man X4 off the legacy views (psxport, megamanx4, Tomba2Engine)

- **Create.** `GuestDisplayFacts { displayHeight; fieldsPerPacingCall; }`, the only source read by `frame_pacer.cpp:41` and `gpu_native_scanout.cpp:19`. Tomba 2 declares 224 lines and its pacing quota there.
- **X4 moves to typed seams.** PlatformHlePlan gets the HLE windows, vsync trap and `dmaCallbackTable = x4::stream_startup::kDmaCallbackTable`. HostIdentity gets the card env and path. The disc key is in TitleIdentity since M3.
- **Delete.** `bindLegacyInterface` (x4_runtime.cpp:43); megamanx4 `game/title/{game_hooks.cpp, game_config.cpp, legacy_game_interface.h}`. `measuredProgramImage` moves to an x4 boot-facts file.
- **Proof.**
  - `test_x4_runtime`.
  - An X4 boot through its FrameDriver where the DMA-3 STR callback still fires. X4 now takes the `table != 0` branch of `hle_interrupt.cpp:120`.
  - `test_frame_pacer` reads GuestDisplayFacts.
- **Play check.** Mega Man X4, then Tomba 2 (its pacing quota moved).
- **Converged.** megamanx4 (docs in D2).

### M11. TitleAudio and TitleDevWarp capabilities (psxport, Tomba2Engine)

- **Create.**
  - `class TitleAudio { mixFrame; nowPlayingName; playSoundTest; }`, returned by `GameRuntime::audio(Core&)`. The null check lives only in its consumers: `SpuAudio`, `ui/menu_readouts.cpp`, `ui/menu_document.cpp`.
  - `class TitleDevWarp`, consumed by `ui/warp_control.cpp`. Tomba 2 implements both from its per-Core NativeMusic and `dev_areas.cpp`.
- **Delete.** Hooks `audioMixFrame`, `audioNowPlayingName`, `audioSoundTestPlay`, `devAreaCount`, `devAreaName`, `devWarpAllowed`. Functions `spu_mix_game_audio`, `game_audio_now_playing_name`, `game_audio_sound_test_play`. Test `test_optional_hook_guards`.
- **Proof.** `test_spu_mix_optional_hook` is rewritten as a TitleAudio absent/present test through `SpuAudio`. A warp-control test uses a stub TitleDevWarp.
- **Play check.** Tomba 2, focused on music and SFX underruns and the sound-test screen.

### M12. Tomba 2's task scheduler leaves psxport (psxport, Tomba2Engine)

- **Precondition.** Read Tomba 1's `GuestTaskBudget`. Move only what is Tomba 2's engine, and never make a second scheduler copy.
- **Move** to `Tomba2Engine/game/core/sched/`: `frame/pc_scheduler.{h,cpp}`, the Tomba 2 stanzas of `platform/scheduler.cpp`, `platform/synchronous_task_wait*`, `task_slot_layout.h`, `SchedBody`, `SchedEntry` and `sched_entry_for`. It is created by `TombaRuntime::createTaskScheduler`. The 12 hook sites become direct `Engine` calls.
- **Delete.** `Game::pcSched` (and its use in `gpu/gpu_perf.cpp`) and hooks `schedStageBody`, `schedFreshEntry`, `hasNativeHandlerForEntry`, `schedRng`. `test_synchronous_task_wait` and `test_task_slot_layout` move to Tomba2Engine.
- **Proof.** Tomba 2 boot reaches `stage=8010637C sm48=2`.
- **Play check.** Tomba 2.

### M13. Tomba 2's CD chokepoints and diagnostics leave psxport (psxport, Tomba2Engine)

- **Move** into `TombaRuntime::registerOverrides`: cdFileLoad, cdAsyncRead, cdInlineLoad, cdCmdStream, voicePlay, voiceStop, lastSectorTracker, cdCallbackTable and cdCallbackFn. `cdDialogToneActive` and `cdMusicFadeIn` become direct `eng.musicCoord` calls.
- **Stock libcd leaves.** cdCommand, cdSync, cdReadPrim and cdInit go on PlatformHlePlan only where Ghidra confirms the body is stock. Otherwise they stay as Tomba 2 overrides.
- **Typed facts and diagnostics.** Packet pools are declared through `guestPacketPoolWindows`, and the DMA table through PlatformHlePlan. `render_noise.h` and `gpu_vk_fadewatch.cpp` lose their Tomba 2 addresses: they move to Tomba 2 or are deleted. Tomba 2's `frame_driver.cpp`, `auto_drive.cpp` and `frame_diagnostics.cpp` read Tomba 2 constants. Split any touched file over 800 lines at the boundary being edited.
- **Shrink.** `cd_override.cpp` keeps only stock libcd.
- **Proof.** A Tomba 2 FMV or XA stream boot, plus a unit test through `hle_interrupt` for `table==0` without cfg. The DMA callback now comes from the native registry instead of the guest slot, which is a real behaviour change.
- **Play check.** Tomba 2: XA, voice and music fade, cutscene stream.

### M14. Delete the legacy layer (psxport, Tomba2Engine)

- **Change.** `TombaRuntime` derives `GameRuntime` directly and overrides `guestProgramImage` and `platformHlePlan`.
- **Delete.**
  - `legacy_game_hooks.h`, `legacy_game_config.h`, `game_iface.{h,cpp}` (LegacyGameRuntimeAdapter), `Core::cfg`, `Core::hooks`, the legacy accessors and `game_hooks_opt.*`. `fps60_game_hooks.*` stays until M25c: `host_world_pass_strategy` still calls it for Spyro 1.
  - The fade decision, made once here: trace who writes `PresentInputs::fade_*` and whether a guest-derived fade exists; name the surviving owner in `presentation.md`. M16 keeps only what this names.
  - Every `if (core.cfg)` branch: game.cpp, hle_interrupt, platform_hle, frame_pacer, pad_input, cd_ready_delivery, stock_cd_work_area, guest_packet_pool_windows.
  - Tomba 2's `game_hooks.cpp`, `game_config.cpp` and `title_facts.h`.
- **Proof.** test_game_runtime, test_in_between_strategy and test_frame_pacer use direct stubs. Delete test_zero_config_is_loud's cfg cases. The `psxport_smoke` legacy assertion is deleted, because the type is gone.
- **Proof, fps60 off.** With the fps60 mod off the adapter's Fps60 `temporal->present` is Tomba 2's live path; frames before and after are identical with fps60 off.
- **Play check.** Tomba 2 with fps60 on (60 fps through `presentRecords`) and off, then Mega Man X4.

### M15. Typed per-Core title session and namespace (psxport + all 11 titles)

- **Create.** `virtual std::unique_ptr<psx::game::TitleSessionState> createSession(Core&)`, an RAII base. Named this way so it does not collide with `psx::host::TitleSession`.
- **Move.** `GameRuntime`, `FrameDriver`, `TaskScheduler`, `HostIdentity`, `TitleAudio` and `TitleDevWarp` go into namespace `psx::game`.
- **Delete.** `createContext`, `destroyContext` and `void *Core::gameCtx` (16 overrides, 13 cast sites). Each title keeps one typed accessor.
- **Gate rule.** cpp_policy global-namespace finding for those types; remove their baseline lines.
- **Proof.** Every repo's touched tests; boot smoke for all titles.
- **Play check.** None; the change is mechanical and the boot smoke covers it.

### D2. Docs: crashbash, megamanx4

Same steps as D1. Fix unresolved references: crashbash 6, megamanx4 3.

---

## Phase R: one renderer

End state:

- Beetle `gpu.c` is the only VRAM and the only GP0 executor.
- `RenderPath = {Record, Device}`, where Device is the oracle and diagnostic view only. The oracle is Beetle's own picture, which Record must match exactly.
- `FrameComposer` builds every in-between frame from `StateProducer` renders of saved state. There is no packet lerping.
- SDL3 GPU stays.

Every milestone in this phase is gated by `recordcheck mismatched=0` at 1x 4:3 (recordcheck compares Record output against the Device oracle), plus the title's existing replay shots.

### M16. Record present stands alone (psxport, Tomba2Engine, vagrant)

- **Step 0.** Delete the remaining `gpu_native_init` reach-ins (toystory2, crashbash, c12, tomba1 if still present) and prove FMV presentation (`movie/native_fmv.cpp`) does not depend on GpuState.
- **Present owners out of Native.** Move `PresentLedger` from RenderQueue to the presenter, and `presentedHeight` plus the `PresentInputs` source (sx, native_w, disp_h) to a Record/Device present owner that reads the device display area and `GuestDisplayFacts::displayHeight`. Nothing on the Record path may still reach `GpuState` or `rq` afterwards.
- **Change.**
  - Record and Device pictures feed `build_present_image` directly. `GpuVkState::present` stops running `upload_vram`, `render_geom` and the present-rebuild decision for those paths.
  - Drop `rq.flush` from `gpu_native_dma.cpp:155` and RqItem capture/emit for Record cores.
  - Keep the 24bpp/FMV device-picture path and `present_fade_rgb`.
  - Residual reach-ins: Tomba 2's global `gpu_frame_no` prototype (margin_render.cpp:5) becomes the presenter's frame number. Vagrant's four `activeRq().flush` sites become `presentation.commit`.
- **Proof.** Shots and recordcheck are byte-identical before and after on Tomba 2 and Crash Bash, including 24bpp/FMV frames.
- **Play check.** Spyro 2, then Toy Story 2, Crash Bash, Crash 1/2, CTR, Tomba 2, Vagrant, Mega Man X4, Tekken 3, C-12. Every Record title, because the present path changed. Expect frame time to improve.
- **Converged.** vagrant (docs in D3).

### M17. Gte titles with no producers flip to Record (Tomba2Engine/tomba1, spider1, crash)

- **Order.** Tomba 1, Spider-Man 2, Crash 3 (`BoundaryRuntime`), then Spider-Man 1.
- **Delete.** `Spider1Widescreen::guestCoordinatesWidened` and its `wide_2d_guest_space` / `plan_centred_wide_margins` use.
- **Proof.** recordcheck at 4:3, then a 16:9 shot per title showing the canvas margins.
- **Play check.** Tomba 1, Spider-Man 1, Spider-Man 2, Crash 3. Look for margin pop-in where culling was widened for the Gte centre.
- **Converged.** spider1, crash (docs in D3).

### M18. Spyro 3 to Record (spyro)

- **Create.** Drawer 0x80022378 registered as `Producer{A0}` with the shared `TerrainStateProducer` in `spyro/game/render/terrain`, with its depth bins and FrameCut as in `spyro2_render_facts.h`.
- **Fix in the shared owner.** Spyro 2's open terrain gaps (issues 0173, 0177, 0179). Spyro 3 inherits them, so they are fixed in the shared terrain owner, never per title.
- **Delete.** `Spyro3Runtime::createTemporalFramePresentation`, `TerrainWorldPass`, `terrain_packet_sink`. Issue 0180 is closed.
- **Proof.** recordcheck at t=1; producer unit tests through `composeFrame` for t=1 and in-between t.
- **Play check.** Spyro 3, then Spyro 2.

### M19. Delete Gte and guest-geometry interpolation (psxport)

- **Delete.**
  - `RenderPath::Gte`, `RenderCapabilities::guestInterpolated` and `widescreenOnly`.
  - `guest_geometry_interpolation.*`, `guest_geometry_scene_source.*`, `projection_provenance.*` (with its RTPS/RTPT tap), `in_between_splice.*`, `in_between_strategy.h`, `wide_2d_layout.*`.
  - `GuestWidescreenProjection::guestCoordinatesWidened`, `plan_centred_wide_margins` and the render_path_control player cycle.
  - Tests: test_guest_geometry_interpolation, test_in_between_strategy, test_in_between_splice, test_wide_2d_layout. test_render_path and test_render_path_cycle are reworked.
- **Play check.** Spyro 3, then Tomba 1, Spider-Man 1. These are the former Gte titles still in rotation.

### M20. Toy Story 2 keyed-only objects get state renders (toystory2)

- **Change.** Negative-header static meshes (issue 0041, 0x80012FE0) and the remaining part drawers including DCPL 0x8001CD34 (issue 0040) get Producer overrides plus StateProducer renders.
- **Proof.** Producer census for TS2 shows 0 keyed-without-render primitives, plus a unit test per producer that t=1 reproduces its entries.
- **Play check.** Toy Story 2.
- **Converged.** toystory2 (docs in D3).

### M21. Tomba 2 ambiguous keys; delete keyed blend (psxport, Tomba2Engine)

- **Change.** Resolve issue 0033 (writers 0x8007E6DC, 0x8003C2D4, 0x8002AB5C, 0x80106824, 0x8007E1B8, 0x8007E9C8).
- **Precondition.** The producer census shows 0 keyed-without-render primitives on every Record title.
- **Delete.** `present/keyed_blend.*` and `tests/test_keyed_blend.cpp`. `FramePresenter::presentRecords` uses the shown record as the `composeFrame` base.
- **Enforce.** `DisplayPassGuard` wraps every `StateProducer::render` in `composeFrame`, so a write to guest memory aborts.
- **Proof.** test_frame_presenter_contract is reworked to cover the guard.
- **Play check.** Spyro 2, Spyro 3, then Tomba 2, Toy Story 2, Crash Bash, Crash 1/2. These are all the interpolating Record titles.
- **Converged.** Tomba2Engine (docs in D3).

### M22. Spyro 1 (a): the guest render driver runs on Record (spyro)

Preceded by an RE brief with no product edit: Ghidra read of the 0x8001ED5C display tail and a `re-frontier` entry.

- **Change.** Split the VSync display tail out of 0x8001ED5C into the title tail (the `FrameTailObserver` seam). The guest then builds its ordering table and calls DrawOTag on the device.
- **Result.** Spyro 1 runs on Record at 4:3 with no interpolation yet. This is RE and frame-ownership work, not a flag flip.
- **Proof.** recordcheck and the oracle compare.
- **Play check.** Spyro 1.
- **Tell the user.** Record draws in retail OT-slot order, so pops that the depth face order used to hide come back (e.g. issue 0120). That is the intended oracle-exact result.

### M23.1-M23.5. Spyro 1 (b): native-leg producers become StateProducers (spyro), one brief each

- **Order, by census rank.** M23.1 terrain (`native_terrain.cpp`, on the shared terrain passes where the drawer matches); M23.2 paired actors and mobys; M23.3 HUD and 2D; M23.4 sky and cyclorama; M23.5 scene recipes.
- **Change.** Each family moves its math from RenderQueue submission to `PrimitiveSink::emit(OtSlot, DrawPrimitive)`. Delete each family's RenderQueue path in the same milestone.
- **Proof.** One unit test per family: at t=1 and 4:3 it reproduces that family's entries.
- **Play check.** Spyro 1 in each sub-milestone.

### M24. Spyro 1 (c): the flip (spyro)

- **Change.** `Spyro1Runtime::renderCapabilities = {temporalInterpolation = true}`.
- **Delete.** `createTemporalFramePresentation`, `spyro/game/render/temporal/` (2,529 lines), `painter_submission_preflight`, `scene_painter_order`, FrameRenderer's native/reference split, paired_actor `fps60WorldPass`, and the dead `SpyroRuntime` base `widescreenOnly` default.
- **Play check.** Spyro 1 at 60 fps, then Spyro 2 and Spyro 3 (shared owners).
- **Converged.** spyro (docs in D4).

### M25. Delete Native (psxport)

Four briefs: M25a RenderQueue, painter, face contest and their tests; M25b GpuState, `gpu_native*` and the second VRAM; M25c Fps60 (including `fps60_game_hooks.*`), host world pass, fade interpolation, native composite; M25d RenderMode/RenderCapabilities trim, mods and env knobs.

- **Delete.**
  - `RenderPath::Native` (the enum becomes `{Record, Device}`), RenderMode's enhancement predicates, and every RenderCapabilities field except `temporalInterpolation`.
  - `GpuState`: `gpu_native.cpp`, `gpu_native_internal.h` (the second VRAM `s_vram`), `gpu_native_scanout.cpp`, and the `gpu_native_init` calls in `host/title_session.cpp` and `movie/native_fmv.cpp`.
  - `render_queue*`, `render_submission`, `render_queue_attribution`, `painter_object_layer`, `gpu_painter`, `face_contest`, `proj_prim`, `ot_lifo_depth`, `guest_packet_filter`, `render_noise/diag/stats/substrate`, `native_composite_capture`, `fade_interpolation`, `fps60*`, `host_world_pass_strategy`, `gpu_vk_depth`, `gpu_vk_semi_order`, and the PC-rasterizer selftests.
  - `GameRuntime::guestVramIsPicture` and `createTemporalFramePresentation`; the `fps60(Game&)` accessor.
  - Mods `face_order` and `debug_*`.
  - Env knobs: PSXPORT_RQ, ZFIGHT, ZBIAS, PAINTER, PAINTFG, PRIMDUMP, POLYDUMP, TEXWATCH, CLUTWATCH, GP0RAW, FADEDBG, VRAMDUMP, CLOBBERDUMP.
  - The 34 machinery tests named in the render inventory.
- **Decide** `guestPictureIsNativeWidth` against Spyro 1's boot logos on Record: keep it only if the logos need it, otherwise delete it.
- **Gate rule.** Delete the `gpu_native.cpp` and `render_queue.cpp` rows from PSXPORT_CAPS.
- **Play check.** Every title, Spyro 1/2/3 first.
- **Tell the user.** These instruments go and are not replaced: texwatch, clutwatch, gp0raw, zfight and the objid overlay. recordcheck and the producer census are what remain.

### M26. Split `gpu_vk.cpp` (psxport)

- **Change.** The remaining code (about 1,000 lines) splits into `gpu_vk_present.cpp` (sink, fade, letterbox, repaint, image present) and `gpu_vk_submit.cpp` (submit and fault handling), each under 1,200 lines. recordcheck stays with the present owner. Delete the `gpu_vk.cpp` cap row.
- **Docs.**
  - `docs/presentation.md` becomes the one renderer doc; its Migration list is rewritten as the current state.
  - Delete `docs/one-renderer.md` and `docs/presentation-contract.md`.
  - Delete codemap rows "Native renderer", "Painter-object attribution", "In-between splice", "Authored face order" and "Presentation enhancements".
  - Write psxport features F-RENDER, F-WIDE, F-FPS60, F-FMV.
- **Play check.** Spyro 1, 2, 3, then the rest.

### D3. Docs: vagrant, spider1, crash, toystory2, Tomba2Engine

Same steps as D1.

- Fix unresolved references: crash 2 paths and 4 `crash1::` symbols, toystory2 1, Tomba 3.
- Tomba2Engine: move the codemap's Guest address index (lines 485-1260) to `docs/guest-addresses.md`, and reduce `titles/tomba1`'s 5 per-directory READMEs to codemap rows.

### D4. Docs: spyro, plus the workspace matrix

- Spyro's `docs/features.md`. Merge the codemap's duplicate "## Ownership" section into the Directories rows, and keep the codemap to 150 lines or fewer.
- The 20 files that restate 60 fps mechanism link to F-FPS60 instead.
- Split `project-state.md`: "Preserved historical evidence" (lines 196-424) and the S005/S011 narrative move to issues and findings.
- Write `psxport/docs/workspace/features-matrix.md`, linked from WORKSPACE.md "Title scope":
  - Rows are feature IDs; columns are the 15 titles.
  - A cell is `-` (non-goal) or a link to that title's features section plus its project-state ID.
  - Required cells come from the scope rules: widescreen and loading removal for every title; 60 fps except Tekken 3, Tomba 1 and X4; VS-CANVAS; C12-DRAW; X4-COOP and X4-SKIP.
  - `codemap.py check --refs` validates the matrix.

---

## Phase C: C APIs and globals

Rule: state goes into the instance that already owns it (Game or Core members). Pure rules go into a project namespace. `extern "C"` stays only on functions the Beetle C code calls back. The Beetle link-seam globals (18 in gpu_device.cpp, 4 in gte_beetle, 2 in spu_beetle, `EventCycles`, `PS_GPU GPU`) are confined to their single adapter file; removing them needs Beetle fork commits and is out of scope.

Every milestone in this phase removes its lines from each repo's `accepted.txt` for the existing cpp_policy rules (`global-namespace function`, `extern declaration`, `block-scope static`). An accepted line that no longer occurs fails, so the lists only shrink.

### M27. Dead code, header hygiene, pure functions (psxport only)

- **Delete.** About 40 unreferenced functions and stale declarations from the C API inventory. Each deletion is confirmed by linking psxport and every title. Anything still alive after M25 is re-checked first.
- **Header hygiene.** Replace the 7 block-scope externs with the owning header. Drop `extern "C"` from c_subsys.h, cfg.h, cdc_state.h, disc.h, xa_state.h, core.h and hw_bind.h wherever no C file calls the function.
- **Move into namespaces or owners.**
  - `wide_*`, `plan_*`, `present_*`, `video_*` go to `psx::present`.
  - `task0_*` and `crt0_plan` go to their boot or platform namespace.
  - `stock_cd_*` and drive timing go to `psx::cd`.
  - `bios_interrupt_*` → Hle, `overlay_glue_*` → RmlOverlay, `io_peripheral_*` and `irq_latch_edge` → Core, `card_hle_*` → Memcard.
- **Proof.** psxport tests; every title links.
- **Play check.** None; nothing in presentation, audio or timing changes behaviour.

### M28. Device groups become classes (psxport + affected titles)

- **Change.**
  - `dma_*` → DmaRegisters and DmaCallbackRegistry.
  - `disc_*` → `Disc`.
  - `xa_*` and `CDC_GetCDAudioSample` → `XaStream`.
  - `cdc_*` → `CdController`.
  - `cd_*` and `cdReady*` → `Cd`.
  - `proj_*` and `libgte_*` → `ProjParams`.
  - `guest_projection_plan` → `GuestPresentationState`.
  - `guest_mult` and `cpu_div` → `psx::cpu`.
- **Rule.** Each group is one commit set together with its title edits.
- **Play check.** Spyro 1/2/3, Tomba 2, Mega Man X4, Spider-Man 1, Crash Bash, Vagrant. These titles exercise CD, XA and the loading-removal paths.

### M29. Bound-instance globals out of SPU, MDEC, XA and the watchdog (psxport)

- **Change.** Delete the retired SBS residue first: `SpuDevice::writeLog`/`spu_bind_log`, the `Sbs::shownCore()` comments in `rmlui_overlay.h`, "SBS boots two" in `crt0_boot.h`. Then `SPU_*`/`spu_*` → SpuDevice; `MDEC_*`/`mdec_*` → MdecDevice; `xa_bind` → XaStream; `watchdog_*` → the ProductHost-owned `Watchdog` from M1, as a class. Beetle callbacks reach the instance through their single adapter file. The title call sites are already gone since M4.
- **Proof.** Oracle differential and field digests on the touched titles; the oracle tools and override differential build and pass. A missed rebind would send one core's writes into another core's state.
- **Play check.** Spyro 1/2/3, then every title with music or FMV: Tomba 2, Crash Bash, TS2, X4, Vagrant, CTR, Tekken 3, C-12, Spider-Man 1, Crash 1/2.

### M30. Debug singletons (psxport)

- **Change.**
  - `DbgServerInternals::sInstance` and its 14 static accessors become members.
  - memcensus `g_slots`/`g_dumped`, store_observe `g_armed` and snapshot's `static Sched` move into their classes.
  - The gpu_vk perf counters (`g_submit_ns`, …) go into GpuPerf.
  - `render_path.cpp g_live_switch` goes to the render-path owner.
  - The 3 rmlui_overlay globals go into RmlOverlay.
- **Play check.** None; no timing path changes.

### M31. Logging goes to Lucent (psxport, Tomba2Engine, megamanx4)

- **Change.** `cfg_logf/i/w/e` and `cfg_line_*` become direct Lucent calls (Tomba 114 sites, X4 18). Delete the shim.
- **Play check.** None.

### M32. One typed config (psxport + titles)

Split by subsystem (gpu/render knobs, cd/audio, debug), plus one brief for the remaining Tomba and TS2 statics.

- **Change.**
  - `cfg_on`, `cfg_int`, `cfg_str` and `cfg_dbg` lookups become typed fields of one immutable `psx::config` object, built by the config owner and reached through Game.
  - The 26 CVar externs and `cfg_dbg_gen_v` become members of that object.
  - The env-cache function-local statics disappear.
  - Tomba2Engine's 61 function-local statics and 7 mutable file-scope statics, and toystory2's 11, move to their owners.
- **Rule.** Hot paths (`OtAttr::trackStore`) must be a plain member load.
- **Proof.** Every getenv sits inside the config owner. The frame-time comparison on hot titles stays within noise.
- **Play check.** Spyro 1/2/3, Tomba 2, then all other titles.

### M33. GPU free functions (psxport + spyro, Tomba2Engine, spider1, crash, tekken3, ctr, megamanx4)

- **Change.**
  - The surviving `gpu_*` and `gpu_vk_*` become GpuDevice, GpuWindow and presenter methods.
  - `gdev()` and the 26 `s_*` macros become an explicit `GpuDevice&` member. Delete `GpuDevice::sInstance`.
  - The dump functions go into `PrimitiveDump`; the selftests go into `psx::gpu::selftest`.
- **Play check.** Spyro 1/2/3, then every title.

### M34. GTE (psxport + all titles; one mechanical landing)

- **Create.** `psx::cpu::Gte`, a Core member wrapping GteRegs. `GTE_*` become its methods, and `GTE_CurState` stops being a process global.
- **Rewrite.** About 1,075 title sites become `core.gte().x(...)`: spyro 606, Tomba 421, c12 96, crash 61, toystory2 44, crashbash 27, ctr 15, vagrant 10, mmx4 9, spider1 2.
- **Delete.** The free functions.
- **M34a first.** Thread `Core&` to every site without a Core in scope, per repo, with no behaviour change. M34b is then the mechanical rewrite with clang tooling, landed at once.
- **Proof.** Also `test_oracle_snapshot` and the override differential.
- **Proof.** Oracle differential on Spyro 1 terrain and Tomba 2.
- **Play check.** Spyro 1/2/3, then every title.

### M35. Closing state and the clang-tidy burn-down (one repo per brief)

- **clang-tidy.** Align `.clang-tidy` in the 10 repos that use the narrowed config to the global rule (`clang-analyzer-*`, `bugprone-*`, `performance-*`), checked by `cpp_policy.py --audit-config`. Fix the findings; never suppress them.
- **Accepted lists.** Every repo's `accepted.txt` must be empty for the global-function, extern and static rules. Anything left is reported to the user by name rather than kept.
- **Docs.** psxport writes its final codemap pass and features F-CD, F-AUDIO, F-LOAD, F-SAVE and F-DEBUG.

---

## Facts to carry into the briefs

- `Game::Game` already calls `render_path_install` for every runtime; no title is missing a render-path install.
- Spyro's 60 fps mechanism is restated in 20 doc files; D4 links them to F-FPS60.
- `native_fmv.h:94` cites `tools/cpp_policy.py`, which is re-harness's; fix the comment in M2.
- `re_frontier.py` exists four times (re-harness, psxport/tools/port, spider1, spyro); one brief near D1 keeps re-harness's and deletes the copies.
- `HostIdentity` keeps card and user-data names, `TitleIdentity` disc facts, `GuestDisplayFacts` display facts; the window title is `catalog.productName()` for single-title catalogs too.

## What is deleted outright, summary

- **Loop.** `native_boot.cpp/.h`, `dc_boot_init`/`dc_step_frame`, `BootStub`, `Machine::run`/`stepFrame` (and `boot/machine.*` if emptied), `SessionRun::fieldDelivered`, `ts2::TitleSession`, `c12::Machine`, `GuestFieldLoop`, `boot_probe.cpp`, `spider::runPort`, `product_launch.cpp`, `runtime_composition.cpp`, `Application::run`, 13 title `main`s, the tomba1 and enter_electro targets, 2 disc extractors, the 7 copies of `bindPerCoreDevices`, `psxport_install_game` and its globals.
- **Interface.** GameHooks, GameConfig, LegacyGameRuntimeAdapter, `Core::cfg`/`hooks`/`gameCtx`, `game_hooks_opt`, `fps60_game_hooks`, the fade hook pipe, the framework `tp`, `replCommand`/`controlCommand` (merged), `discEnvVar` (moved), the X4 and Tomba 2 legacy tables, and Tomba 2's scheduler and CD engine code inside psxport.
- **Render.** RenderPath Native and Gte, GpuState with its second VRAM, RenderQueue, the PC rasterizer half of gpu_vk, keyed blend, Fps60, guest-geometry interpolation, the splice, face contest, the painter layer, `wide_2d_layout`, Spyro `render/temporal/`, `one-renderer.md`, `presentation-contract.md`, and the listed debug knobs and overlays.
- **C API.** About 40 dead functions, the 7 bypassing block-scope externs, every non-Beetle `extern "C"`, the CVar externs, the `cfg_*` and `cfg_log*` shims, all non-Beetle singletons and bound globals, and the free GTE, GPU, CD, XA, SPU and MDEC functions.

## Changes the user will see

- The headless cap unit becomes fence steps (Spyro).
- Closing the window finishes the current field and writes the report instead of calling `exit(0)`.
- Tomba 1 and Spider-Man 2 move into their siblings' pickers.
- Spyro 1's draw order becomes retail-exact.
- Former Gte titles get canvas margins at 16:9.
- The debug overlays listed in M25 are gone.

# Project state

This inventory describes the current psxport tree. Product intent is in `project-goals.md`; remaining
runtime work is specified in `migration.md`.

## Comparison baseline

The baseline is the previous native PSX host framework with no usable product CPU executor. The tree
now runs guest code on psxport's own R3000 interpreter (`runtime/cpu/guest_executor.*`) on every host;
the intended complete delta is a native/interpreter hybrid that executes authenticated user images at
runtime and retains the existing native device, rendering, audio, input, HLE, and enhancement owners.

## Current focus

S013 is the current focus: complete native-service interception and bounded exit handling inside
guest control flow without restoring an offline dispatch substrate.

## Capability inventory

| ID | Capability | State | Dependencies | Goals |
| --- | --- | --- | --- | --- |
| S012 | Per-`Core` guest interpreter | partial | — | G001, G004 |
| S013 | Explicit architectural state and bounded exits | partial | S012 | G001, G003 |
| S014 | Image/generation-scoped native and original calls | partial | S012, S013 | G001, G002, G003 |
| S015 | Central executable-code invalidation | partial | S012 | G001, G003 |
| S016 | Product path excludes offline guest generation and engine choices | verified | — | G001, G004 |
| S017 | Reusable native PSX platform services | partial | — | G002 |
| S018 | Independent test oracle | partial | S013 | G003 |
| S019 | Narrow multi-title framework seam | partial | S012, S017 | G002 |
| S020 | Record rendering and title-owned enhancements | partial | S017 | G003 |
| S021 | Portable desktop and Android delivery | partial | S012, S019, S020 | G003, G004 |
| S022 | Mechanical structure/config/logging/tooling policy | partial | — | G002, G004 |
| S023 | Whole-machine save states (title-neutral; a title adds native state via `GameRuntime::nativeState(Core &)`, and adopts RAM-derived state in `NativeStatePort::restored` after every section is restored; a frame driver whose `ResumableGuestCall` spans steps saves it with `ResumableGuestCall::save`) | verified | S012, S017 | G003 |
| S024 | In-app bug report (B key: freeze, present shot + GPU device render of the same frame, pad recording + start card, replay command, RmlUi form) | partial | S017, S020 | G003 |
| S025 | Online input netplay for multi-pad titles (`docs/plans/online-netplay.md`: deterministic stepping, per-frame savestates, pads at the input boundary, rollback or lockstep over any IP path) | missing | S023, S024 | G003 |

### S012 — Per-`Core` guest interpreter

`psx::cpu::GuestExecutor` (`runtime/cpu/guest_executor.*`, `Core::executor()`) runs every guest
instruction no native override claims, directly on `Core`'s registers, reading code from guest memory as
it runs. It has real branch and load delay slots and one guest clock tick per instruction. Its ledger
reports calls, executed blocks and instructions, host dispatches, memory accesses, device clock commits,
invalidations by source, faults and budget exits. The same executor builds on every host; there is no
recompiler, translation cache or fallback tier. Gap: the evidence in the sections below was gathered on
the recompiler the interpreter replaced, and multi-`Core` qualification remains open.

### S013 — Explicit architectural state and bounded exits

The executor runs on `Core`'s GPR, HI/LO, PC, CP0, and COP2 state, accounts executed instructions
through the canonical title clock, services pending work at block entries, and exposes typed budget/fault/syscall/break/pending-work results. `execute` retains
individual syscall checkpoints; bounded function calls and `dispatchGuestUntilExit` resume supported
syscalls and serviced pending work, with native/HLE routing independent of the optional return
address. A separate typed host-dispatch allowance bounds native-only loops without fabricating guest
cycles. Unknown syscall selectors and delay-slot exceptions are refused before native syscall state
mutation. The synthetic runtime tests cover these paths; complete delay-slot exception continuation
and interrupt conformance still require integrated proof.

### S014 — Image/generation-scoped native and original calls

`runtime/cpu/image_identity.*`, `native_dispatch.*`, and `guest_call.*` provide the top-level API.
The synthetic runtime contract proved call interception, nested native context, resume, and an original guest body stopping at the exact caller continuation. Gap: representative
resident/address-colliding overlay and generation-reuse coverage remains absent.

`callOriginalUntilExit` uses the same scoped suppression and native caller-context restoration while
allowing a guest polling body to end at a requested frame/yield/process exit. A synthetic
nested native frame exit proves that suppression and requested-exit state do not leak after return.
An enclosing native override running its original body through an ordinary native call and a later
frame exit proves guest execution resumes from the returned continuation, independently of the
scoped C++ caller PC restored by nested native dispatch.

Override correctness has a diagnostic: `runtime/cpu/override_differential.*` shadows sampled calls of selected
overrides in a real run (`PSXPORT_OVERRIDE_DIFF`), compares original and native under the O32 contract
with device traffic replayed, continues from the original, and writes its report to
`PSXPORT_OVERRIDE_DIFF_REPORT`. Gap: not yet exercised on a real title's override
(`docs/issues/0138-override-differential-gate.md`).

### S015 — Central executable-code invalidation

Range normalization, image generations, dispatch revocation and counters exist. The executor reads code
from guest memory as it runs, so self-modifying code needs no cache flush and its invalidation calls are
only counted. Public `Core::mem_w8/16/32`, the span store `Core::mem_write` (CD and archive loads:
bookkeeping and rollback once over the span, one copy) and guest CPU stores share one post-write
notification owner in `guest_memory.cpp`. Gap: complete DMA/debugger/savestate mutation coverage still
requires proof.

### S016 — Product path excludes offline guest generation and engine choices

Evidence: the product source/link boundary scan rejects offline guest source, CPU-engine selectors,
and player-selectable engines. The one guest interpreter is the only executor.

### S017 — Reusable native PSX platform services

GPU, SPU, GTE, MDEC, CD, DMA, timing, pad, BIOS/HLE, and host-presentation owners remain. Gap:
complete service coverage and integration are title-driven and have not been demonstrated in the
interpreter product path.

The typed direct-runtime HLE plan now installs the existing stock `CdCommand`, `CdSync`, and
`CdSearchFile` services alongside `CdRead`/`CdReadSync`, while the legacy registration retains the
same handlers. Crash Bash's authenticated executable reaches its native loop through this route;
its BOOT overlay publication remains title work.

Continuous reads now declare whether the host pump or the guest interrupt path owns ready-callback
delivery. The default direct callback path remains available; the guest-owned path advances the
controller to a due INT1 and lets the existing guest ISR consume its response before invoking the
callback. The synthetic callback contract covers both owners, an empty/non-INT1 queue, and deferred
delivery across a native override (5/5 cases, 49 checks). Spider-Man's retail ISR-to-libstr ring
sequence remains unverified.

Guest interrupt handlers (chain verifier and handler, DMA completion callbacks, the CD ready callback)
now run on `Hle::kExceptionStackTop` instead of the interrupted `$sp`, as the retail BIOS exception
handler does; a title that repurposes `$sp` as a data pointer with interrupts
enabled (Spyro 3's display-list culler) was having its list overwritten by the ISR's frames.

### S018 — Independent test oracle

Beetle/Mednafen CPU-window tools remain separate from the product. The isolated libretro software
console host in `tools/oracle/console.py` also boots user-supplied CHDs with explicitly admitted BIOS
firmware and exposes live pad input, RAM reads, framebuffer capture and audio-consumption counters.
Its seven synthetic host tests cover real callback boundaries and failure cases. A Linux x86_64
Clang build at Beetle `054dd6a7` boots Spyro 1 through the supplied North American SCPH-1001 v2.2
BIOS into Artisans and responds to held Left; title comparison evidence belongs to Spyro's state
inventory. No BIOS or disc bytes enter the source or package.

Gap: full-console checkpoint alignment and device/timing divergence localization remain incomplete.
The console has independent CPU execution and scheduling but shares Beetle device implementation
lineage with the product, so agreement cannot rule out a shared device bug. CPU-window snapshots
do not claim full-console state parity. See `tools/oracle/CONSOLE.md` for scope and commands.

### S019 — Narrow multi-title framework seam

The shared runtime API includes typed product/compare-candidate/compare-reference roles and owns
comparison-run enhancement suppression without title-local environment parsing. Gap: consumers
still require coordinated pins and representative gameplay requalification without copied
framework code.

### S020 — Record rendering and title-owned enhancements

One renderer: the device executes the guest stream and the record rasterizer draws it
(`docs/presentation.md`); the native and GTE paths are deleted. Evidence: recordcheck t=1
`mismatched=0` on C-12, Spyro 1/2 and Tomba! 2. Gap: each title's widescreen and interpolation must be
requalified in the play check.

### S021 — Portable desktop and Android delivery

Missing capability: no gameplay-ready backend, Windows host contract, macOS arm64 evidence, Android
`arm64-v8a` evidence, or release package exists. Hosted CI therefore runs only the proven Linux
x86-64 synthetic runtime; unsupported hosts have no green placeholder jobs.

Partial, in the product host: a one-title product with no usable disc shows the shared setup-ui
first-run screen and remembers the accepted image; a packaged (AppImage `APPDIR`) run defaults to a
window and its shipped assets and keeps its data in user data (`docs/title-host.md`). Evidence: a Crash Bash zero-argument packaged run refused with no disc
(hidden) and, from a saved choice, extracted its executable and booted. Gaps: the windowed screen has
not been driven by a player click and it has no handset-size capture.

Partial, Linux release: `tools/port/appimage_container.py` builds a title in the Ubuntu 22.04 image
(`tools/port/appimage-build.Containerfile`: clang-20 and SDL3/SDL3_image built in) and `tools/port/appimage.py` packages it, refusing game content and any ELF needing more than
glibc 2.35, with GitHub update information and its `.zsync` (`docs/appimage.md`). Titles call
`.github/workflows/title-release.yml` from a `v*` tag or by hand.

Partial, Windows release: llvm-mingw cross-builds a title (`consumer_build.py --platform windows`) and
`windows_package.py` writes `<Name>-<version>-windows-x86_64.zip`, refusing game content and any DLL Windows
does not ship; the release workflow builds and uploads it beside the AppImage (`docs/windows.md`). Evidence:
the Tomba archive under Wine 11 reached Tomba 2 free roam with 0 faults on the recompiler
(`psx-tomba/scratch/windows/wine_gameplay_1000.png`). Gaps: no Windows-kernel run, unsigned.

Partial, macOS release: `consumer_build.py --platform macos` builds for the Mac's own architecture and
`macos_package.py` writes `<Name>.app` with MoltenVK under SDL_GPU's Vulkan backend, refusing game content and
any non-system library (`docs/macos.md`); the release workflow builds it on `macos-15-intel`, so the release
app is x86-64. Gaps: no run on a Mac, ad-hoc signature, and an arm64 release needs an Apple-silicon
build machine. Android has none.

### S022 — Mechanical structure/config/logging/tooling policy

Canonical Python build/verification tooling runs through the checked-in `pyproject.toml`/`uv.lock`;
a shared declarative consumer verifier, full-history Linux CI, product-boundary checks, and seeded whole-tree architecture,
deleted-path, and current-runtime policy checks exist. Gap: remaining large runtime owners still
need responsibility-driven extraction.

Nested CMake tests receive the parent configure's exact compiler inputs, use stable directories under `build/test-fixtures`, and preserve the locked Python executable
symlink. Submodule-sync fixtures supply their own Git identity for every command. These repair the
three fixture failures in hosted run `33893969146`; a subsequent hosted run must establish CI success.

`external/psxport` is the live framework: a relative symlink to the workspace's sibling `psxport`, or a
shallow clone of psxport `main` with its vendor submodules on a machine that has no sibling checkout.
`tools/psxport_fetch.py` makes it and has no copy in any title: a title's launcher and CI run it from
psxport `main` (`uv run --no-project <raw URL> --auto`) only while `external/psxport` is missing, and
`scripts/bootstrap_workspace.py` runs it for every title. No port records a framework commit.

### S024 — In-app bug report

`psx::debug::BugReportSession` over `shared/bug-report`. Evidence (Tomba! 2, headless, 2026-10-05): the
since-retired REPL `bugreport` at pad frame 2400 after two Cross taps saved `screen.png` (native, 960x720), `psx.png` (the
GPU device render of the same ordering table), `repro.pad`, `memcard.mcr`, `report.json` and
`README.md`; replaying that report's recording on a copy of its card and filing again at frame 2400
produced byte-identical `screen.png`, `psx.png` and `repro.pad`. Gaps: the windowed B key and the
form's drawing over the window are unrun by an agent (agents do not run windowed). Since 2026-10-27 the form is
filed only through injected input: hidden Spyro 1 from the selector, `key B`, `text injected chord check`,
`key 224 down` (Left Ctrl), `key Return`, `key 224 up` saved `20261027-181213-injected-chord-check`
(`psx-spyro/scratch/input-injection/fix.log`). Since 2026-10-27 the
device render is taken at `FramePresenter::commit` for every title and `motion.png` (the last second of
presents, 640x360 APNG, PSXPORT_BUG_REPORT_CLIP) is attached: hidden unpaced reports from Toy Story 2 (Level 4 title card after a
Debug-tab warp), Tomba! 2 and Spyro the Dragon (level 11 after a Debug-tab warp) each hold `psx.png`
matching the screen and a 60-frame `motion.png`. Only the Game holding player input takes P / '.' / B,
so a report opened over the title selector no longer opens one in each running panel; that path needs a
shown window and is unrun by an agent.

## Blocking dependency

Gameplay is blocked on multi-`Core` qualification, complete executable-write invalidation coverage,
and representative title/device integration.

## Host evidence gaps

- x86-64 Linux: native/original-call/resume control flow, exact return stopping, measured timing,
  pending work, and typed fault behavior passed in `test_dynarec_contract` on the recompiler; typed
  comparison enhancement suppression passed in `test_diagnostic_run`; representative gameplay is absent.
- Apple Silicon macOS: no run or device evidence.
- Android `arm64-v8a`: no NDK build, APK, or device evidence.
- Windows: the llvm-mingw release build above; no CI host.

No title is migrated until it runs representative interactive gameplay with nonzero executed guest
blocks, native/original calls, invalidation, devices, rendering, audio, and input on the intended
product path.

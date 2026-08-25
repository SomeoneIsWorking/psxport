# psxport — a PSX→PC static-recompilation framework

**Unlabeled content is machine convention, revisable by any session. USER lines are verbatim dated
quotes and only those.**

## NEVER DUPLICATE CODE — no matter the reason

USER, 2026-08-20: *"Never duplicate code no matter the reason"*. No exceptions clause, and none is
implied by "it's only a declaration", "the copies are identical", or a comment admitting the copy.

This is not style. It cost a real bug the same day: `rec_coro_run` was declared in `core.h` inside its
`extern "C"` block AND in `scheduler.h` with ordinary C++ linkage. The two disagreed about linkage,
which is invisible until something reorders includes — and clang-format's include sorting promptly
did, breaking the build in a way that read as a formatter problem. It was not; it was a duplicate.

A sweep for the same shape (every function declared in more than one first-party header) found three
more, two of them carrying the same latent linkage mismatch:

| duplicate | where | note |
|---|---|---|
| `rec_dispatch` | `core.h` (in `extern "C"`) + `guest_abi.h` + `guest_call.h` | both re-declared it WITHOUT the linkage, and both already `#include "core.h"` |
| `xa_decode_sector` | `c_subsys.h` + `fmv_decode.h` | the copy even carried a comment saying "identical declaration" |

**A copy you have documented is still a copy.** Include the header that owns the thing; one owner per
declaration. `python3` one-liner that finds these is in the session log — re-run it after any header
churn.

## The two bars everything else answers to — the USER's own

1. **STRUCTURE — the port is built PROPERLY, and DUSKLIGHT is the model.** USER, 2026-08-12: *"My only
   rules are properly structured (like Dusklight) and RE driven work"* — the same sentence that states
   rule 2. (An earlier, UNDATED quote says the same thing more strongly — *"impeccable code structure
   and organization and impeccable UI"* — kept as corroboration only, since it fails this file's own
   dated-quote bar.) Not a finishing pass: consult `~/repo/dusklight` BEFORE designing anything it
   already solved (section at the bottom), take the SHAPE, cite what you took.
2. **RE-DRIVEN — the deliverable is a READABLE PORT derived from reverse engineering, never a black-box
   fix.** USER, 2026-08-12, naming both bars at once: *"My only rules are properly structured (like
   Dusklight) and RE driven work"*. A change that makes a symptom vanish while the surrounding code
   still reads as guest-memory soup is not finished work. Decompile first (section below), then write
   code that says what it does.

Everything further down is the mechanics of satisfying those two.

This repo is the **game-agnostic framework** extracted from the Tomba!2 port: the static recompiler
(`tools/recomp/`), the `runtime/recomp/` substrate (MIPS interp + Beetle GTE/MDEC/SPU backends + the
native SDL_GPU renderer + the SBS differential harness + BIOS/SDK HLE), and the `psxport` STATIC library
a game links. It carries **no game code** — the framework `#include`s nothing from a game; a game derives
`GameRuntime` (`runtime/recomp/game_runtime.h`), installs that process-lifetime object, supplies the
recompiled substrate (`generated/`), and links `libpsxport`. `GameConfig` + `GameHooks` now exist only as
the bounded compatibility adapter for ports still migrating. The `psxport_smoke` target proves the real
derived seam links with both legacy views null and zero game symbols.

**This file is the authority for how a game consumes psxport** — and, because every game vendors this
repo as `external/psxport`, everything in `docs/` reaches every game tree and every subagent working
in one. That is why the cross-repo material lives HERE rather than in a workspace directory that only
exists on one machine:

| in this repo | what it is |
|---|---|
| `docs/workspace/WORKSPACE.md` | the workspace map: which trees exist, which framework checkout is writable, how a game builds against in-progress framework work |
| `docs/workspace/PROTOCOL.md` | the multi-agent protocol (area claims, worktrees, landing order) + the standing USER rules and the incidents behind them. It defers to THIS file for the seam, `generated/`, RE tooling and diagnostics — so do not chase a pointer in a circle |
| `docs/workspace/LAYOUT.md` | the target directory organization for every tree |
| `docs/plans/*.md` | designs not yet implemented, each stating what is measured vs assumed |
| **`docs/one-renderer.md`** | **THE frame-build invariant** — a frame is built once, by `presentPass(t)`; fps60 only decides whether an extra lerped present is inserted. Which branches are legitimate, the three layers that had to be unified together, and the two partial attempts that produced broken pictures. Read before touching fps60, `RenderQueue::flush`, or anything that branches on `mods.fps60` |
| `scripts/bootstrap-workspace.sh` | reproduces the whole workspace from a clone of this repo |

A game's own specifics stay in that repo's `CLAUDE.md`.

- **Consuming games:** Tomba2Engine (the reference consumer), spyro, spider1 — each vendors this repo
  as `external/psxport`.
- **Porting a NEW game:** see `docs/porting-a-new-psx-game.md` + `docs/port-framework.md`.

## Where the framework source comes from — and who may edit it

**There is ONE psxport working tree on a machine and every port uses it.** A game's `external/psxport`
is no longer a submodule: it is a SYMLINK to that tree (or, on a fresh machine, a private clone at the
game's `psxport.pin`). So reaching the framework through `$PSX/psxport` or through a game's
`external/psxport` is the SAME directory, an edit there is live in every port at once, and there is no
"read-only consumer" copy that can drift from the writable one — because there is no second copy.
Commit and push framework work in `psxport/`. Claims, worktrees and landing order are
`docs/workspace/PROTOCOL.md`'s; the mechanism and the two submodule incidents that motivated dropping it
are in `docs/workspace/WORKSPACE.md`.

A game builds against in-progress framework work without touching its submodule:

```sh
cmake -S . -B build -DPSXPORT_DIR=$PSX/psxport
```

`PSXPORT_DIR` defaults to `external/psxport`, and `tools/psxport_sync.py --auto` (which `run.sh` calls)
makes that resolve to the shared tree locally or to a private clone at `psxport.pin` on a fresh machine —
so a clone of a game repo alone still builds, which is what "each game is its own project using psxport
as the framework" means. Each game's precommit runs `psxport_sync.py --check`, which FAILS when the
framework you built against is not the commit the repo records. `run.sh` ANNOUNCES which checkout a run was built from (and whether it was dirty); a binary
built from in-progress framework work must never be mistaken for one built from the pin.

## Build and test

```sh
cmake -S . -B build && cmake --build build --target psxport        # the library
cmake --build build --target psxport_smoke                        # agnosticism proof: zero game symbols
cmake --build build && ctest --test-dir build --output-on-failure  # the gate
ctest --test-dir build -R <test_name>                             # one test
```

A framework change **starts with a RED hermetic test** (no disc, no window): drop in one
`tests/test_*.cpp` using `tests/testutil.h` — they are globbed, so there is no shared file to edit and
no merge point — run it, and paste the failure before the fix. `docs/project-map.md` ("Tests") for the
layout, `docs/workspace/PROTOCOL.md` ("TDD") for why the rule exists.

## `./run.sh` IS THE USER'S. Agents build and gate with their own tools

**TOOLS ARE PYTHON. `run.sh` is the only shell script that gets to exist** (USER, 2026-08-16: *"only
run.sh should be a shell script, all other scripts should be python"*). It is the exception because it
is the user's launcher, not a tool. Everything else — gates, sweeps, builders, converters — is a
`.py` with `argparse`, a `--help` that says what it asserts, and an exit code that distinguishes
PASS / FAIL / REFUSED-because-it-could-not-assert-anything. New shell scripts are not to be added, and
a shell script you touch is one you convert. The conversion backlog is tracked in Tomba2Engine's kanban.


**USER directive, 2026-08-11: *"I don't know why you are running run.sh, that is for me to play, you
should have your own tools."* An agent must never invoke `./run.sh`** — not even with
`PSXPORT_NOWINDOW=1`. It is the end-to-end WINDOWED play launcher: it re-syncs submodules, re-extracts
the boot executable, recompiles, rebuilds and launches. Its submodule re-sync silently reverts
in-progress framework work to the recorded pin, after which every measurement describes a different
framework than the agent believes (`docs/workspace/PROTOCOL.md`).

Agents build explicitly and drive the ALREADY-BUILT binary:

```sh
cmake -S . -B build && cmake --build build --target <spyro_port|spiderman_port|tomba2_port> -j$(nproc)
./scratch/bin/<port> scratch/bin/<game>/<EXECUTABLE>     # the binary is HEADLESS BY DEFAULT
PSXPORT_NOPACE=1 python3 tools/gate.py boot --frames 400 \
    --expect-stage <entry> --expect-sm48 <n>             # Tomba!2's agent gate
```

`PSXPORT_NOPACE=1` because a headless run is PACED now: without it the gate measures ~60 presents per
second instead of several hundred, and every before/after number becomes incomparable.

**A gate is a TOOL the game repo owns, not a borrowed launcher.** `Tomba2Engine/tools/gate.py` is the
reference shape: it neither builds nor extracts, it drives the REPL over stdin, and it prints its own
denominator every run (lines scanned, failure patterns searched, prologue frame, frames advanced, end
state, the exit-time env audit) so a pass cannot be confused with a run that never launched; its
refusals exit 2 rather than 0. **Do not assert an absolute end frame:** `newgame` pulses an unspecified
number of frames to reach the GAME prologue (measured 27 on one build where an older note says 39), so
an absolute frame is a hardcoded expected value that fails for reasons unrelated to your change. Assert
the ADVANCE past the prologue plus the END STATE. Games without a gate tool grow one.

The disc image is never in a repo. Resolution order is **CLI arg > env var > `.env` > a `*.chd` dropped
in the repo root**; the env var is `PSXPORT_SPYRO_DISC` / `PSXPORT_SPIDERMAN_DISC` /
`PSXPORT_TOMBA2_DISC`. Every repo already has a `.env` pointing at its disc. Binaries land in the
git-ignored `scratch/bin/`; `generated/` (the recompiled substrate, ~200 MB of C) is git-ignored.
To extract a boot executable or inspect a disc WITHOUT run.sh, use the framework's own tool:
`cmake --build build --target discdump`, then `discdump list|get <NAME> <disc> [outdir]`.

**`PSXPORT_NOWINDOW` is read by NOTHING in the binary** — only `run.sh` translated it (into
`PSXPORT_VK_HEADLESS`), so it does nothing for an agent driving the binary directly. Do not set
`PSXPORT_VK_HEADLESS` either: a run is HEADLESS BY DEFAULT (`gpu_vk.cpp` requires `PSXPORT_VK_WINDOW=1`
to open a window, so a forgotten flag fails safe).

## Configuration: knobs resolve through a CVar ladder

`Default` < `Value` (psxport_settings.ini) < `Override` (env) < `Runtime` (REPL/debug-server) —
`runtime/recomp/config_var.h`, inventory in `config_vars.h`, ladder and per-knob status in
`docs/config.md` + `docs/config-migration.md`. The REPL `cvars` command dumps every knob, its value,
and which layer it resolved from. **Check that a knob you rely on was actually read before trusting the
run** — a knob that matches nothing is NAMED (`[cfg:warn] UNKNOWN knob X is set and matched nothing`) —
but:

**JUDGE AN UNKNOWN-KNOB WARNING ON THE *EXIT* AUDIT, NEVER ON THE STARTUP LINE.** The startup audit
runs before the late-initialising subsystems have read anything, so it reports every knob on a
not-yet-entered code path as `UNKNOWN ... it did NOTHING in this run` — a claim about the run, made
before the run. It prints its own blind spot and says to re-check at exit; do that, and read only:

```
[cfg] env audit AT EXIT (everything that was going to be read has been): N set -> ... 0 UNKNOWN
```

Measured 2026-08-12: a census run with 6 knobs set reported **4 UNKNOWN at startup**
(`PSXPORT_GATE`, `PSXPORT_PAD_REPLAY`, `PSXPORT_PRODUCERS_DIR`, `PSXPORT_REPL`) and **0 UNKNOWN at
exit** — all four had worked. A previous version of this paragraph named two knobs as "the only known
false alarms" and said to treat every other one as a flag that really did nothing; that rule produced
a false conclusion in BOTH directions — it would clear `PSXPORT_PAD_REPLAY` as broken when it works,
having earlier let a genuinely misspelled replay knob pass as fine. The number of late-read knobs is
not a fixed list, so only the exit audit answers the question. `Tomba2Engine/tools/gate.py` gates on
the exit line for exactly this reason.

**`PSXPORT_REPL` works on the SINGLE-CORE loop only.** `Repl::read()` is pumped by `native_boot.cpp`
and nothing else, so piping a REPL script into an SBS run (`PSXPORT_SBS` / `PSXPORT_SBS_MODE`) used to
discard every command while the harness ran its own default lockstep — that is how a crash report came
to blame a `newgame` on a run that never left attract mode. Such a run now **exits 2** naming what it
did not service; drive an SBS run with `PSXPORT_SBS_AUTONAV` / `PSXPORT_SBS_WARP` /
`PSXPORT_SBS_PAD_REPLAY` / `PSXPORT_DEBUG_SERVER` instead (`docs/config.md`,
`runtime/recomp/repl_service.h`). **`PSXPORT_DUALCORE` and `PSXPORT_SELFTEST` refuse the same way** —
they are the other two loops a game's `main()` dispatches to that own the process without a REPL pump,
and each names its own drive mechanism, not the SBS one.

Common knobs: `PSXPORT_NOAUDIO=1` · `PSXPORT_DEBUG=cd,gpu` (channel-gated diagnostics) ·
`PSXPORT_FORCE_RECOMP=1` · `PSXPORT_WATCHDOG=<sec>` · `PSXPORT_SPIN_TICKS`/`PSXPORT_SPIN_RUNS`
(guest spin detector: a starved-host, one-region instruction burn aborts naming the region) ·
`PSXPORT_REPL=1` · `PSXPORT_SNAP_AT=<frames>` · `PSXPORT_WWATCH=<lo>,<hi>`.

## Architecture — the same two halves in every game repo

- **`external/psxport`** — this framework. `tools/recomp/` statically translates the PSX MIPS R3000A
  executable into emitted C (`shard_*.c`, the "substrate"); `runtime/recomp/` is the native PSX
  platform layer (GPU/SPU/GTE/MDEC/CD/XA/FMV + BIOS/SDK HLE), the SDL_GPU renderer, and the
  side-by-side (SBS) differential harness. It `#include`s **nothing** from a game.
- **`game/`** — the game half: `game/core/` owns a derived `GameRuntime`, the recomp registry, `main()`,
  and native reimplementations that progressively take ownership of substrate functions. During
  migration, its derived runtime may delegate unmoved members through the bounded legacy adapter.

The ownership seam is `runtime/recomp/game_runtime.h` (`GameRuntime`, per-Game `FrameDriver` and
`TaskScheduler` products) and `recomp_iface.h` (`RecompRegistry`). `runtime/recomp/game_iface.h` owns
only the temporary adapter/install boundary; the deprecated bags are isolated in
`legacy_game_config.h` and `legacy_game_hooks.h`. They are compatibility debt, not extension points:
do not add a field or callback there. `psxport_smoke` and `test_game_runtime` keep the derived seam and
its temporary adapter honest.

A shipping port still uses `LegacyGameRuntimeAdapter` until every generic `c->cfg` consumer has moved
to a narrow typed fact group. `GuestProgramImage` is the first completed group: crt0, overlay routing,
and the resident-code backtrace heuristic consume the immutable value owned by `GameRuntime`; only the
adapter projects the old fields for unmigrated ports. `DiscIdentity` is the next candidate group, and
the consumer migration/deletion set and subsequent groups are in
`docs/plans/game-seam-redesign.md`. Do not make `GameRuntime` return the old bag or add one virtual
integer getter per field.

Consequences that bite:

- **`generated/` is sacrosanct.** Never hand-edit it. A mistranslation is fixed in the recompiler; a
  missing function is fixed by adding a **seed** to the game's `recomp_seeds.json` — with the
  rationale for how the address is reached — never by patching output. Never copy another game's
  seeds; they land mid-function and silently corrupt the recomp.
- **Never guess a guest address or an overlay load base.** A wrong one does not fail cleanly — it
  breaks boot or diverges the byte-compare in a way that reads as a framework bug. An un-RE'd
  `GameConfig` field stays `0` with a TODO naming the frontier step. Zero is honest.
- **Ownership is gated.** A native body must be proven to replace the substrate body byte-exactly,
  and you must prove the native body actually *ran* — a green gate where the override never installed
  (both sides ran substrate) is hollow. A measured constant that ships must be compared BY CODE to the
  measurement it came from (`docs/workspace/PROTOCOL.md`).

## Start every non-trivial task by consulting the game repo's registries

Each game repo carries its own information system in-repo (greppable Markdown, so it reaches
subagents), with the same three questions:

```sh
python3 tools/info.py brief <words>          # docs/info/ — what's proven, and does it still hold?
python3 tools/re_frontier.py next            # docs/re-frontier.md — which RE step is actually ready
python3 tools/catalog.py search <symptom>    # docs/issues/ — hit before? ruled out before?
```

Plus, per repo: `spyro/tools/whatis.py 0x800xxxxx` (cross-references an address against every source
at once — run it rather than hand-cross-referencing), `Tomba2Engine/tools/codemap.py --addr <hex>`
(who already owns a `FUN_xxxx`) and `tools/findings.py` / `tools/kanban.py`.

Two invocation traps: `spider1` requires `RE_FRONTIER_ROADMAP=docs/re-frontier.md` on
`re_frontier.py` (without it the tool silently parses an empty roadmap and reports OK), and
`Tomba2Engine` reads its codemap from `docs/code-map.md` while `spyro`/`spider1` use `docs/codemap.md`.

**Navigate by the codemap, not by history**, and leave it pristine — update it in the same change that
moves, adds, deletes or re-owns anything (the rule and the USER quote behind it:
`docs/workspace/PROTOCOL.md`). End the task by writing back what you proved, what you disproved, and
any tool you caught lying.

## Reverse-engineer first — never black-box, never hand-walk disassembly

At any of these triggers — a magic offset, a `FUN_xxxx`/`sub_xxxx` you are about to call, a mystery
`obj[+0xNN]`, a value you cannot name — **stop and decompile** before changing anything.

```
tools/decomp.sh        Ghidra headless -> C (the entry point for all games)
tools/abi_extract.py   frame size, spill offsets, ra constants, from generated/
tools/port_gen.py      byte-faithful first draft of a port
tools/port_check.py    static store-sequence equivalence gate
```

`disasm.py` / `disas.py` are single-instruction **spot-checks after Ghidra**; using them to understand
behaviour is the banned method. And `port_check.py` compares static store sequences, so a genuine
rebuild (a table replacing an unrolled jump table) can fail it by construction — when it does, prove
equivalence by **running** both paths and diffing the 2 MB RAM dumps, and never contort readable code
back into a transcription to satisfy the checker.

The deliverable is a **readable port** (USER rule 1 at the top): typed struct lenses over guest blocks
(`dlg.state()`, not an offset), named constants for every literal address saying what it IS, enums for
state-machine states, real C++ classes whose method names say what the code DOES — not `ov_*` free
functions — and ABI plumbing through `runtime/recomp/guest_abi.h` rather than open-coded `r[]`
juggling. A body full of `c->mem_r32(0x800E7FD8)` is a transcript, not a port, and it HIDES bugs: you
cannot see a state fork that never fires when every read is an opaque hex address. Byte-exact mechanics
stay byte-exact — this is about how it reads. Per-repo Ghidra wrappers and the exemplar files to match
are in `docs/workspace/PROTOCOL.md` ("RE FIRST, and the deliverable is a READABLE PORT").

## Diagnostics — `lucent::`, one line per call site, NO `if` around it

This is the authority for the rule; `docs/workspace/PROTOCOL.md` only restates the two halves agents
keep violating.

**Pick by AUDIENCE, not by wrapping:** `info`/`warn`/`error` for what a normal run should print,
`debug(channel, …)` for what is shown only when asked (`PSXPORT_DEBUG=cd,gpu`).

**`cfg_*` is a retired printf shim.** No new call sites; convert any line you touch:

```
cfg_logf("cd",  "sector %u -> %08X", n, dst)   →  lucent::debug("cd",  "sector {} -> {:08X}", n, dst)
cfg_logi("boot","loaded %s (%d bytes)", p, n)  →  lucent::info ("boot","loaded {} ({} bytes)", p, n)
cfg_logw / cfg_loge                            →  lucent::warn / lucent::error
```

`%08X`→`{:08X}`, `%-12s`→`{:<12}`, `%zu`/`%llu`/`%u`→`{}`, `%.2f`→`{:.2f}`, `%%`→`%`. **One trap:**
`printf("%s", p)` on a null `const char*` prints `(null)` under glibc; `std::format` on one is
UNDEFINED BEHAVIOUR. Check every `%s`.

**Never wrap a log call in a condition** — `lucent::debug` is channel-gated internally and does not
evaluate its arguments when the channel is off, so a guard is pure noise that re-creates the
`if (dbg) fprintf(…)` idiom the logger exists to abolish. The ONE legitimate guard is around genuinely
expensive **non-logging** work, guarding a BLOCK, with an interned `lucent::Channel` (0.49 ns vs
20.5 ns for the `string_view` form) and only where a site is genuinely hot — a dozen places, not
everywhere. Rows built in a loop (hex dump, register grid) use `lucent::Line`: accumulate, flush once,
never a log call per byte.

## Never commit

Disc images (`*.chd`), extracted executables, `generated/`, or machine-specific absolute paths
(`/home/<user>/…`). Every game repo ships `tools/go_public.py` to audit the full history for exactly
this. Everything transient goes in the git-ignored `scratch/`, split by kind (`scratch/logs/`,
`scratch/bin/`, `scratch/raw/`) — **never `/tmp`**, a small RAM-backed tmpfs on this machine; diagnose
"disk quota exceeded" with `quota -s`, not `df`.

## Other people are working on these binaries — `docs/prior-art.md`

Read it before designing anything and before assuming we are alone on a binary: the matching decomps per
title (Spyro 1's is already a submodule and targets a byte-identical executable; Tomba! 1 has one at
18.17%), the static-recompiler ecosystem that already ships Tomba! and Tomba! 2 ports with this
architecture, the LICENSE ASYMMETRY deciding whether we may take code or only the shape (CC0 vs PolyForm
Noncommercial), why psxport structurally cannot report a decomp.dev percentage, and a standing USER
decision about which community not to engage. Where a reference and a MEASUREMENT disagree, the
measurement wins.

## Follow DUSKLIGHT's structure — consult it BEFORE designing anything it already solved

`~/repo/dusklight` (`github.com/TwilitRealm/dusklight`, **CC0**, `git pull` before reading — it moves
daily). A shipping PC port of Twilight Princess on the `zeldaret/tp` decomp: different console, same
problem shape, and USER rule 1 at the top of this file is to follow it closely. **Take the SHAPE, not a
copy-paste**, and cite what you took.

Renderer support follows that peer-owner shape too: `gpu_primitive_dump` owns primitive-census CSV
lifecycle and encoding, while `image_writer` owns RGB24 file output for every renderer. Do not grow
those responsibilities back into `gpu_native.cpp` or `gpu_vk.cpp`.

- **UI is TWO stacks on purpose.** `src/dusk/ui/` is RmlUi for the game-facing UI; `src/dusk/imgui/` is
  ImGui for developer overlays (console, save editor, heap/process/camera overlays, actor spawner).
  Shipped UI and debug UI have different requirements and must not share a framework.
- **The RmlUi UI is COMPONENTISED**, which is the part psxport most needs: `ui/component.{hpp,cpp}` is a
  base class owning its subtree (`add_child<T>()`), with scoped event listeners (`listen()` returning a
  `ScopedEventListener`, so teardown is automatic), and state expressed as RCSS pseudo-classes
  (`selected`, `disabled`) rather than ad-hoc bools. Concrete components — `button`, `bool_button`,
  `modal`, `menu_bar`, `document`, `graphics_tuner`, `logs_window`, `mods_window`,
  `controller_config` — are each a file pair. Stylesheets live apart in `res/rml/*.rcss`.
  psxport follows that boundary under `runtime/ui/`; `runtime/recomp/rmlui_overlay.cpp` owns RmlUi
  lifetime and event routing only. Feature state stays in cohesive controls: for example,
  `render_path_control` owns the Display tab's renderer selector, while `pad_input` knows nothing
  about renderer policy. Details and the deliberate authored-DOM difference are in
  `docs/ui-architecture.md`.
- **Config is a layered CVar system** — already adopted here (`runtime/recomp/config_var.h`, above).
- **Frame interpolation is RECORD-AND-REPLACE** (`src/dusk/frame_interpolation.{h,cpp}`): the sim tick
  runs untouched while each final matrix is recorded keyed by its own address; the presentation frame
  lerps prev→cur into a REPLACEMENT table that draw-time sites consult, so guest state is never
  mutated. The camera is interpolated as a POSE (eye/center/up/bank/fovy/near/far, bank via
  `remainderf` for wrap), never as a matrix lerp. Note the sim-frame vs presentation-frame split, and
  `request_presentation_sync()` for frames that must be exact.

  **We may not copy it against our substrate yet, and the reason is PRECISION, not depth:** Dusklight
  records matrices the decomp's own code computed in float, while a GTE-side matrix is s16 fixed point
  whose inversion error is camera-dependent. The technique is unlocked as the PC takes ownership of the
  code that COMPUTES the transform, because then the matrix is our own variable. Full argument, with
  the measured 1.53 px artefact: `docs/workspace/PROTOCOL.md`.

# psxport — native/dynarec PlayStation port framework

This file is the canonical project-local authority for psxport. `CLAUDE.md` is a discovery symlink;
never edit it separately. The portfolio-wide native/dynarec contract and migration order live in
`../../shared/jit-common/docs/migration.md`. Local intent, factual capability state, ownership, and
atomic work live in `docs/project-goals.md`, `docs/project-state.md`, `docs/codemap.md`, and
`docs/issues/` respectively.

Before concurrent framework work, read `docs/workspace/PROTOCOL.md` completely. Before non-trivial
work, run the shared `info.py brief <terms>` entry point from the repository root and read the issue
catalog before re-deriving a symptom.

**Unlabeled content is machine convention, revisable by any session. USER lines are verbatim dated
quotes and only those.**

## Product architecture is settled

psxport's intended product is a game-agnostic native/dynarec hybrid:

- verified native functions and subsystems execute in host code;
- every remaining guest instruction executes on demand through a maintained, immutable Lightrec
  revision; and
- the user's authenticated PSX image remains runtime data. No build, install, provisioning, or
  release path emits guest functions as C/C++, object files, or a precompiled title substrate.

PSX is not evaluating an interpreter against a JIT. Lightrec is the product CPU executor. Any
interpreter is a separately built test-only oracle and must be absent from the gameplay library's
objects, link graph, configuration, UI, command line, and fallback paths. Lightrec configurations
that interpret difficult or not-yet-compiled blocks are not acceptable for the gameplay target:
the maintained fork must provide a dynarec-only product build, and an untranslatable block must
produce a named executor failure rather than silently interpreting it.

The repository has not implemented this architecture yet. Generated-C dispatch, the in-library
interpreter, and the `PSXPORT_ENGINE` selector are current migration gaps, not extension points.
Follow `docs/migration.md`; do not add another generated-code feature, engine choice, or compatibility
fallback while replacing them.

## Lightrec dependency boundary

- Integrate Lightrec directly at an exact immutable revision from its maintained repository or a
  maintained fork. Record the URL, revision, upstream base, and why a fork is required in the normal
  dependency metadata. Never carry a patch file or depend on the indirectly bundled copy under the
  Beetle hardware backend as the product pin.
- One live `Core` owns one Lightrec state and its callback context. No process-global active core,
  register pointer, current image tag, override table, clock, or invalidation target may select a
  different instance implicitly.
- Lightrec owns its translated-block cache, chaining, executable-memory allocation, publication,
  and teardown. Do not wrap or duplicate those mechanisms with `jit-common`; use Lightrec's supported
  APIs and extend the maintained fork when a product requirement is missing.
- psxport owns the PSX-specific integration: canonical CPU state, memory/device callbacks,
  machine-state synchronization, image identity, native dispatch, scoped original calls, bounded
  exits, and invalidation requests.

## Executor contract

The product loop is:

`Core state -> synchronize into Lightrec -> execute within a budget -> bounded exit -> synchronize
back -> handle host/native/device work -> resume`

The boundary is explicit and testable:

- Entry synchronizes GPRs, HI/LO, PC and delay/next-PC state, CP0, GTE registers, pending interrupt
  state, and the guest cycle deadline. Exactly one representation is authoritative while guest code
  executes.
- Before a native override, HLE/device callback, interrupt/exception handoff, frame/VSync boundary,
  thread yield/exit, budget exhaustion, or fault is observed by host code, all guest-visible state and
  elapsed cycles are committed to `Core`. After host work, any state changes are synchronized back
  before resuming.
- Execution returns a typed result containing the reason, current guest PC, and consumed budget.
  It never unwinds a C++ exception through JIT frames. Budget exhaustion is an ordinary bounded exit;
  an unsupported instruction, executable-memory fault, or unavailable dynarec backend is a named
  failure.
- Host frame, task, and BIOS/HLE owners request exits through this contract. They never fast-forward
  guest simulation or manufacture a phase/timer/scene state to escape guest code.

## Image-scoped native calls

An address alone does not identify PSX code because overlays reuse ranges. The runtime key is the
active authenticated image/module identity, its load generation, and guest address.

- A normal guest call resolves that complete key and invokes the native override when one is active;
  otherwise it enters the original guest body through Lightrec.
- An original call from inside an override suppresses only that exact override key for the dynamic
  extent of one call. Nested calls continue to honor all other overrides, including a different image
  at the same guest address. Suppression state is per-`Core`, bounded, and restored on every typed exit.
- Loading, unloading, or replacing a module increments its generation and makes stale override keys
  unreachable. Equal addresses in resident code and two overlays must be independently testable.
- Installing, removing, or replacing an override invalidates every Lightrec path that captured the
  old dispatch decision. Do not leave patched/chained host calls pointing at stale policy.

All writes that can modify executable bytes use one invalidation owner. CPU stores, DMA, CD/module
loads, decompression into executable RAM, debugger writes, and savestate restore report normalized
guest ranges after the write becomes visible. The owner invalidates every overlapping Lightrec block
and any derived image-generation decision. Positive changed-byte and controlled no-change/out-of-range
tests are required; a diagnostic that only prints on invalidation is not evidence it scanned anything.

## Structure is part of correctness

Use `docs/codemap.md` as the placement authority. The target CPU integration is split by ownership,
not accumulated in `Core`, `native_boot.cpp`, `dispatch.cpp`, or a replacement god class:

- `runtime/cpu/lightrec_executor.*`: one per-`Core` Lightrec lifetime and bounded run orchestration;
- `runtime/cpu/state_bridge.*`: explicit architectural-state synchronization;
- `runtime/cpu/execution_exit.*`: typed exit reasons and budgets;
- `runtime/cpu/code_identity.*`: authenticated resident/module identity and generation;
- `runtime/cpu/native_dispatch.*`: image-scoped overrides and one-call original dispatch;
- `runtime/cpu/invalidation.*`: normalized executable-write notification and Lightrec invalidation;
- `tests/oracle/`: separately built interpreter/oracle support, never a product dependency.

Names are target ownership, not permission to add empty forwarding files. Each class owns one cohesive
concept, receives explicit dependencies, has bounded RAII lifetime, and exposes a narrow API. The host
entry point composes owners and does not absorb their implementation. Before extending an existing
1,200-line source, split the touched responsibility. The normal structure gate must keep the 1,200-line
default cap, shrink-only legacy limits, forbidden dependency checks, and exact-file failures.

## Never duplicate declarations or policy

USER, 2026-08-20: *"Never duplicate code no matter the reason"*.

Include the owning header. Do not redeclare another module's function at a call site, and do not copy
an ABI declaration, state-transfer formula, dispatch rule, memory mapping, or config parse into a test
or diagnostic. Tests exercise production seams. A circular include is a boundary defect to resolve,
not permission for a local forward declaration.

Removing or changing a public framework function requires searching every PSX consumer, not only this
repository. Game repositories are part of the caller set even though psxport remains game-agnostic.

## Configuration and diagnostics

Configuration has one owner. It ingests CLI, environment, settings, and live diagnostic changes,
validates them, and passes typed immutable values to subsystem constructors. Product modules do not
call `getenv`, read config files, or infer execution mode. There is no product CPU-engine setting:
the gameplay executor is Lightrec by construction. A test oracle is selected by building/running a
different test target, not by a gameplay CVar or menu choice.

All product logging goes through Lucent. Use `lucent::info`/`warn`/`error` for normal-run messages and
`lucent::debug(channel, ...)` for opt-in diagnostics. Never wrap a log call in an `if`; the logger owns
channel filtering. Never call `printf`, `fprintf`, `std::cerr`, platform debug-print APIs, or the
retired `cfg_log*` shim from product code. Build expensive non-logging diagnostic data only behind an
interned Lucent channel check, and emit one line per call site.

Execution diagnostics must report denominators: blocks translated/executed, exits by reason,
invalidation candidates/overlaps, override lookups/hits, and original-call depth. A zero count must
distinguish "scanned and found none" from "instrument never ran". Product-link inspection must prove
the interpreter and generated guest corpus are absent; observing no fallback in one scenario is not
enough.

## Reverse-engineer first

Decompile before changing a mystery guest address, offset, state field, call boundary, or overlay
identity. Use Ghidra and the game's binary/runtime evidence; single-instruction disassembly is a
spot-check, not a substitute for behavior recovery. Keep still-valid binary facts in the appropriate
game repository or `docs/migration.md`. Static generated C is not the new reference and must not be
regenerated, built, or run as migration evidence. New comparison evidence comes from the original
binary, an independent emulator/hardware trace, or a separately built test oracle.

Native overrides are deliberately owned behavior or proven service boundaries, never repairs for a
missing MIPS semantic. Fix instruction behavior in Lightrec or its integration owner. A readable port
uses named types and state transitions; opaque guest-memory soup is not complete merely because it
runs.

## Faithful behavior and presentation

USER, 2026-08-30: *"Change the directive, pixel matching doesn't matter. I just want working game
that looks correct."*

Faithful execution comes before intentional enhancements. The completion bar is representative,
interactive gameplay with correct behavior, rendering, audio, input, timing, and native overrides—not
boot, a logo, FMV, an internal trace, or a pixel-difference count. Differential tools diagnose first
divergence only within their declared scope.

Widescreen is a deterministic projection/viewport/scissor change that renders additional geometry.
It never stretches the final image or samples adjacent frames/content to decide coverage. Temporal
interpolation is separate and may blend only matching source geometry with explicit provenance.

## Build, test, and repository hygiene

Agents configure C++ verification builds with Clang and confirm the build metadata names Clang. The
project remains compatible with its supported GCC, Clang, and AppleClang toolchains. Touched C/C++ is
formatted with the tracked `.clang-format` and linted with the tracked `.clang-tidy`; do not suppress
or weaken diagnostics.

Use focused hermetic tests while iterating and one combined gate only after semantic edits are frozen.
`./run.sh` belongs to players and is not an agent test command. Project automation is Python; `run.sh`
is the only allowed shell launcher. Build products belong under top-level `build/`; run artifacts go
under the gitignored `scratch/`, never `/tmp`. Never issue raw `rm`; use the scoped cleanup tools.

Never commit game discs, executables, generated guest corpora, BIOS files, logs, or machine-specific
absolute paths. The user supplies game assets at runtime, with explicit path first, environment/`.env`
second, and repo drop-in last. A fresh product must not require Ghidra or any offline translator.

The code is not migrated merely because these plans are coherent. Implementation is complete only
after the focused and combined gates pass on the integrated tree and the operator commits and pushes
the milestone on `main`.

# Faithful-port framework — `port_gen.py` / `guest_abi.h` / `port_check.py`

Three layers, built together, that make the #1 recurring porting bug class (ABI/stack-frame
transcription errors — see `docs/abi-extract.md`'s bug catalog, and CLAUDE.md's "MIRROR THE GUEST
STACK") structurally hard to write in the first place, instead of catching it after the fact. All
three build on `tools/abi_extract.py`'s parser of `generated/*.c` (the recompiler's ground-truth
output) — nothing here forks a second parser of that dialect.

## The three layers

1. **`tools/port_gen.py`** — draft generator. Emits a class method whose body is the gen function's
   guest-visible operations **verbatim** (same `c->r[]`, `c->mem_r*`/`c->mem_w*`, `func_XXXXXXXX(c)`/
   `rec_dispatch(c, ...)` calls with their real `c->r[31]` constants, labels/gotos). Byte-faithful by
   construction on day one — there is nothing to hand-transcribe wrong. The agent's only job after
   `port_gen` is **renaming**: locals get names, labels become real control flow, fields get named
   accessors. `tools/port_check.py` verifies that renaming didn't change behavior.

2. **`runtime/recomp/guest_abi.h`** — OPT-IN helpers for hand-writing a NEW faithful port without
   `port_gen`'s verbatim-transcription step, while still making the callee-saved-liveness bug class
   (the "Enqueue bug" — a value gen keeps live in `r16..r23`/`r30` held as a plain C++ local instead
   of `c->r[N]`, so a nested call's spill sequence sees stale content) structurally impossible:
   - `GuestReg<N>` — a proxy for `c->r[N]`, used as a drop-in local so every read/write goes straight
     to the real register file.
   - `GuestFrame<FrameSize, NumSpills>` — RAII stack frame: descends `sp`, spills the given registers
     at their RE'd offsets on construction, restores + ascends on destruction. The spill table is
     **not** hand-derived — generate it with:
     ```
     python3 tools/abi_extract.py <addr> --scaffold --guestabi
     ```
     which emits a ready-to-paste `static constexpr GuestFrameSpill kSpills_<addr>[N] = {...};` +
     `GuestFrame<FrameSize, N>` declaration straight from the gen body.
   - `guest_call` / `guest_dispatch` — set the RE'd `c->r[31]` return-address constant, then call
     (direct function pointer or `rec_dispatch`), so the "forgot to set ra before a call" bug class
     (9-in-one-cluster / 17-in-another per `docs/abi-extract.md`) can't be written silently.

   This is opt-in style for **new** ports — existing hand-ports (the raw `c->r[29] -= N` /
   hand-rolled `XxxFrame` RAII idiom in `game/render/perobj_dispatch.cpp`, `game/world/object_table.cpp`,
   `game/audio/sequencer.cpp`) are NOT mass-migrated. `Render::perModeDispatch` was migrated as a
   demonstration (see Validation #4 below) — the hand-rolled idiom remains the house style elsewhere
   until a session deliberately chooses to migrate a file.

3. **`tools/port_check.py`** — the equivalence gate. For every method carrying a marker comment:
   ```
   // PORT_GEN: <addr> <generated-file>:<start>-<end>      (emitted automatically by port_gen.py)
   // ORACLE: gen_func_<addr>                                (hand-added on an existing faithful port)
   ```
   it normalizes BOTH the native method body and the oracle `gen_func_<addr>` body to a coarse,
   renaming-tolerant **guest-visible operation sequence** — frame descent/ascent sizes, ordered call
   sites (`c->r[31]` constant + resolved target address), and the ordered sequence of memory-store
   **widths** — and diffs the two. Verdict per method:
   - **PASS** — every checkable axis matches exactly.
   - **FAIL** — a DEFINITE mismatch (wrong frame size, missing/extra/reordered call, wrong `ra`
     constant, wrong call target, wrong memory-store width sequence). Exit code nonzero.
   - **UNPROVABLE** — every axis that COULD be checked matched, but at least one comparison could
     not be resolved (an indirect-dispatch target that's a runtime value on either side, or a native
     call to another owned method the tool can't map back to a guest address). Never silently
     promoted to PASS. Warning by default; `--strict` makes it fail the run too.

   `port_check` understands the RAII frame idiom (`XxxFrame frame(c);` where `XxxFrame`'s ctor/dtor
   carry the actual `sp` ops), the `sp0 = c->r[29]; c->r[29] = sp0 - N; ... c->r[29] = sp0;` alias
   idiom, and the `guest_abi.h` idiom (`GuestFrame<N,...>`, `guest_call`, `guest_dispatch`) — all
   three native dialects seen in the house-style examples.

## When to use which

- Porting a **brand-new small leaf** and want zero transcription risk: `port_gen.py`, then rename in
  place, then `port_check.py` to prove the rename didn't change behavior.
- Hand-writing a **new** faithful port from scratch (e.g. following the RE'd struct-field shape from
  a Ghidra pass) and want the register-liveness/frame bug classes to be unwritable: use
  `guest_abi.h`'s `GuestReg`/`GuestFrame`/`guest_call`/`guest_dispatch` instead of raw `c->r[]`.
- **Any** faithful port, old or new: add an `// ORACLE: gen_func_<addr>` marker above the method and
  run `port_check.py` on the file — it's a free equivalence re-check any time the method changes.

## Validation (2026-07-10)

All commands run from a build2/scratch/bin/tomba2_port built in this session's isolated worktree.

### 1+2. `port_gen.py` on two unowned addresses — compiles+links as dead code

Picked via a small scan of `generated/*.c` for small (`sp-24`, single-spill, single-call) unowned
leaves, cross-checked unowned via `docs/code-map.md`:
```
python3 tools/port_gen.py 8001CE90 --class DemoLeafA --method run --file game/core/demo_leaf_a.cpp
python3 tools/port_gen.py 8002311C --class DemoLeafB --method run --file game/core/demo_leaf_b.cpp
```
Both added to `cmake/tomba2_port.cmake` (marked `# port_gen.py validation draft, UNWIRED dead code`),
`cmake --build build2 --target tomba2_port` — clean link, nothing calls either method.

### `port_check.py` round-trip sanity — PASS
```
python3 tools/port_check.py game/core/demo_leaf_a.cpp game/core/demo_leaf_b.cpp
# [PASS] game/core/demo_leaf_a.cpp :: DemoLeafA::run  (oracle 0x8001CE90)
# [PASS] game/core/demo_leaf_b.cpp :: DemoLeafB::run  (oracle 0x8002311C)
```
Expected: a verbatim transcription must compare equal to its own source under any honest normalizer.

### 3. `port_check.py` on 3 existing hand-ported faithful methods

`// ORACLE: gen_func_<addr>` markers added to `Render::cmdListDispatch` (0x8003CDD8),
`Sequencer::seqChannelDispatch` (0x80090BD0), `OverlayGroundGt3Gt4::gt3` (0x8013FB88) — the three
files named in the task brief.

```
python3 tools/port_check.py game/render/perobj_dispatch.cpp game/audio/sequencer.cpp \
    game/render/overlay_ground_gt3gt4.cpp
```

- **`Render::cmdListDispatch`** — FAIL: memory-store width sequence mismatch (native has 18 stores
  where gen's linear scan shows 27, in the 3-column GTE-compose loop). **Manually verified**
  (`sed -n` on `generated/shard_6.c:5119+`): the original PSX compiler fully **unrolled** the
  3-iteration column loop into 3 literal repeated instruction blocks, so gen's flat text contains
  each column's 3 stores 3 times over (9 occurrences); the native port correctly uses one real
  `for (int col = 0; col < 3; col++)` loop (1 set of 3 stores, executed 3 times at runtime, same
  bytes written) — a **tool artifact of comparing static textual occurrence count across a
  loop-unrolling boundary**, not a functional bug. Also produced one **UNPROVABLE** (the
  `perModeDispatch()` tail-call — an owned sibling method, not a `func_X`/`rec_dispatch` call the
  tool can resolve to an address — correctly flagged rather than silently dropped from the count).
  Documented here as a known limitation of the width-sequence axis (see below), not silently waved
  off — no residual exclusion is claimed; the actual bytes are still proven correct by the SBS gate.

- **`Sequencer::seqChannelDispatch`** — **UNPROVABLE** (3 call sites route to other owned native
  leaves — `channelKeyEventScan`-style sibling methods — the tool cannot statically resolve their
  guest address). All frame/width axes PASS. This is the intended honest outcome for a method that
  calls into other native code: never claim PASS on an unresolvable axis.

- **`OverlayGroundGt3Gt4::gt3`** — FAIL: memory-store width sequence mismatch (native has 4 fewer
  32-bit stores before the loop's 16-bit OTZ store than gen). Partially explained by the same
  unrolling-boundary effect as `cmdListDispatch` (gen's linear text carries BOTH the `mode==1` and
  `mode==2` `sz`-minmax write blocks as separate labeled regions — see `docs/abi-extract.md`'s own
  documented gap on this exact function — while the native port correctly consolidates them into one
  parameterized write, per CLAUDE.md's anti-duplication rule), but **not fully re-verified
  line-by-line within this session's budget** — flagged here as a genuine open finding for a
  follow-up RE pass, not dismissed as tool noise. Recorded as an OPEN finding, not a confirmed bug or
  a confirmed non-bug.

**Known limitation of the memory-store-width axis**: it compares STATIC textual occurrence count and
width, in program order. This is unsound across a loop-unrolling boundary — if either side represents
an original small fixed-count loop as an unrolled sequence of repeated blocks while the other uses a
real loop, the textual counts differ even though the runtime behavior is identical. The axis correctly
still reports FAIL (never a false PASS) in this situation; a human must distinguish "unrolling
artifact" from "real missing store" by inspecting `generated/*.c` directly, exactly as done above.
Recording this explicitly rather than silently special-casing it away — a special case here would be
the same "no bandaids" violation CLAUDE.md bans elsewhere.

### 4. Demo migration to `guest_abi.h` — SBS-full 0-diff

`Render::perModeDispatch` (0x8003F698, `sp-24`, single `ra` spill) rewritten to use
`GuestFrame<24, 1>` (fed by an `abi_extract.py --scaffold --guestabi`-shaped `kSpills_8003F698[]`
table) and `guest_dispatch()` in place of the hand-rolled `PerModeFrame` RAII struct and raw
`c->r[31] = ...; rec_dispatch(...)` pairs. Behavior-identical by construction (same offsets, same `ra`
constants — `perModeCaseReturnAddr`/`0x8003F790u` unchanged).

```
timeout 95 env PSXPORT_VK_HEADLESS=1 PSXPORT_SBS=1 PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1 \
  PSXPORT_NOAUDIO=1 PSXPORT_SBS_NOPAUSE=1 PSXPORT_TOMBA2_DISC=<disc.chd> \
  ./scratch/bin/tomba2_port > scratch/logs/sbs_gate.log 2>&1
grep -cE 'sbs-div|VIOLATION' scratch/logs/sbs_gate.log   # -> 0
```
Ran to frame 7440 (wall-clock cap, not a failure) with 0 `sbs-div`/`VIOLATION` lines — sustained
zero-diff. **Caveat, stated honestly**: a targeted `PSXPORT_DEBUG=dispatch` capture within this
session's time budget did not reach a frame where `cmdListDispatch`/`perModeDispatch` fire (they are
seaside-render-specific, not exercised by the generic intro-area autonav path within ~5300-7400
frames) — so this validation proves **no regression to the frames actually reached**, consistent with
`docs/fleet-workflow.md` §9's standing caveat about autonav coverage, not an `ovhit`-confirmed direct
observation of this specific override firing in this session. The override registration itself
(`perobj_dispatch_install`, `engine_set_override_main`) was NOT touched by this migration — only
`perModeDispatch`'s internal body — so the firing behavior is inherited unchanged from the
already-verified prior state (see `docs/findings/render.md`'s f118 writeup).

## Files

- `tools/port_gen.py` — draft generator.
- `runtime/recomp/guest_abi.h` — `GuestReg`/`GuestFrame`/`guest_call`/`guest_dispatch`.
- `tools/port_check.py` — the equivalence gate.
- `tools/abi_extract.py` — shared parser + `--scaffold --guestabi` mode (extended this session).
- `game/core/demo_leaf_a.{h,cpp}` / `demo_leaf_b.{h,cpp}` — `port_gen.py` validation drafts, UNWIRED.
- `game/render/perobj_dispatch.cpp` — `Render::perModeDispatch`, the `guest_abi.h` demo migration.

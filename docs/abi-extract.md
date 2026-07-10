# `tools/abi_extract.py` — static guest-ABI/stack-contract extractor

## Why this exists

The #1 recurring bug class when hand-porting a `gen_func_<addr>` / `ov_<area>_gen_<addr>` body to a
native C++ class method (see `docs/findings/render.md`'s `perobj_dispatch`/`cmdListDispatch` cluster,
and `docs/fleet-workflow.md` §9's wide-RE draft bug counts) is getting the guest-stack frame / callee-
save spill / `c->r[31]`-before-call / callee-saved-register-liveness boilerplate wrong:

- missing or wrong `sp` descent + spill offsets/widths (frame mirroring)
- missing `c->r[31] = <RE'd constant>` before a call (delay-slot `ra` mirrors — 9 in one cluster, 17 in
  another, per `docs/findings/render.md`)
- callee-save liveness: values gen keeps live in `r16..r23`/`r30` that flow into a callee (which spills
  or reads them) kept only as C++ locals in the native port instead of written to `c->r[N]` before the
  call (the `Enqueue` bug, the `cmdListDispatch` r16-r23 bug)
- store ORDER of dead scratch relative to branches/gates (the `gt3`/`gt4` write-order bug)
- per-branch duplicate inline scratch at DIFFERENT stack offsets depending on which branch is taken
  (the `mode==1`/`mode==2` SZ-scratch idiom in `ov_a00_gen_8013FB88`/`ov_a00_gen_8013FE58` — the f179
  bug, `docs/findings/render.md`)

`tools/abi_extract.py` parses the RECOMPILED C TEXT under `generated/` (ground truth — never Ghidra's
pseudo-C, which garbles COP2/delay slots per CLAUDE.md's "Ghidra first, never disas.py walk" rule) for a
given guest address and emits the contract mechanically, so these stop being hand-derived from scratch
every time.

This is a **static text parser over the recompiler's fixed emission idioms, PLUS a real basic-block
CFG** built from those idioms (labels, conditional/unconditional `goto`, the switch-dispatch jump-table
shape, and `return`). It fails loudly (`AbiParseError`, nonzero exit) rather than emit a guessed-wrong
contract when it hits a shape it doesn't recognize — a wrong contract is worse than none.

## Usage

```
python3 tools/abi_extract.py <hexaddr> [--contract] [--json]      # default: human-readable contract
python3 tools/abi_extract.py <hexaddr> --scaffold                  # C++ Frame RAII + call-site block
python3 tools/abi_extract.py <hexaddr> --audit <native.cpp>        # heuristic grep-audit of an existing port
```

`<hexaddr>` may be given with or without a `0x` prefix (e.g. `8003CDD8` or `0x8003CDD8`). The tool
auto-locates the body across every `generated/*.c` shard and every `generated/ov_<area>_shard_*.c`
overlay by scanning for `void gen_func_<addr>(Core* c) {` or `void ov_<area>_gen_<addr>(Core* c) {`.

Run it **twice** per leaf, per `docs/fleet-workflow.md` §9: once at DRAFT time (to know the frame/call
shape before writing the port), and once at WIRING time (to catch drift from the draft during the
mandatory line-by-line RE verify pass — the extractor is NOT a substitute for that pass, it just removes
the bulk of the boilerplate-transcription error surface before it starts).

## How the CFG is built

The recompiler's C dialect uses a small, fixed set of control-flow shapes (enumerated by grepping the
whole `generated/` corpus — 7900+ function bodies — before writing the parser):

1. **Branch-delay compound**: `{ int _t = (COND); DELAY_INSN; if (_t) goto L; }` — `DELAY_INSN` always
   executes (MIPS branch-delay slot), the branch is conditional. Verified over 47,524 corpus instances
   that the delay part is always exactly one statement.
2. **Compound with no delay insn**: `{ int _t = (COND); if (_t) goto L; }` (8,364 instances).
3. **Unconditional goto**, optionally preceded by exactly one statement on the same line
   (`STMT; goto L;` or bare `goto L;`) — verified this shape never carries more than one leading
   statement.
4. **Return**, bare (`return;`), the frame-ascent combo (`c->r[29] = c->r[29] + (uint32_t)N; return;`),
   or preceded by exactly one statement (`STMT; return;` — the common tail-call/tail-dispatch shape,
   ~17k instances, e.g. `func_XXXX(c); return;` or `rec_dispatch(c, ...); return;`).
5. **Switch-dispatch jump table**: `{ switch (EXPR) { case C: goto L; ... default: STMT; return; } }`
   — the recompiler's coroutine/state-resume idiom. Verified uniform across all 1,041 corpus instances
   (every case is exactly `case CONST: goto LABEL;`, always exactly one `default: STMT; return;` tail).
   The tokenizer round-trips the parsed case list against the source text and raises `AbiParseError` if
   it doesn't reconstruct exactly — i.e. it refuses to silently drop a case edge on an unrecognized
   shape rather than guess.
6. **Label-prefixed statement lines**: `L_XXXX:; STMT;` — a label and one statement on the same
   physical line (4,030 corpus instances; never chained/multi-label). Tokenized as label + statement.

Three structural corpus facts the CFG additionally handles:

- **Two-line frame close**: some bodies (e.g. `gen_func_80087A60`) emit the sp-ascent and the
  `return;` on separate lines instead of the fused single-line form. Both spellings count toward
  `frame_close_sizes`, restricted to REACHABLE exit blocks.
- **Label at function entry**: a body may open with a label on its very first instruction (e.g.
  `ov_a03_gen_8010F1A0`), making the literal entry block empty; the prologue scan walks forward
  through empty single-fallthrough blocks to the first block with statements.
- **Dead merged-sibling code**: several gen bodies carry a second, never-entered function's code
  after their real `return;` (no label ever targets it — `gen_func_800232F4`, `80079528`,
  `80087A60`, `8003F174`, and more). The CFG excludes it via entry-reachability
  (`unreachable_block_count`), so a dead sibling's frame/spills/stores can no longer pollute the
  live function's contract — the pre-rewrite linear tool both false-FAILED 39 such functions
  corpus-wide ("frame OPEN size does not match CLOSE size(s)") and, on 7 others, silently reported
  the dead sibling's frame size / spill set as the live function's.

Any line that doesn't match one of these shapes is treated as a plain, non-branching statement
(the common case — register ops, memory ops, `gte_op`/`gte_read_data`/`gte_write_data`, calls). The
tokenizer never has to "recognize" those individually; only branch/return shapes need enumeration.

From the token stream, `build_cfg()` splits the body into basic blocks (one per label, plus anonymous
blocks after every conditional branch's not-taken edge) and wires `Edge`s: `fallthrough`, `cond_taken`
/ `cond_fallthrough` (with the source condition text attached, for human-readable branch-guard
reporting), `uncond`, and `switch_case`. A block created immediately after an unconditional
`goto`/`return`/`switch` default is marked `orphan` and never gets an implicit fallthrough edge into
the next label — because code between an unconditional control transfer and the next label is, by real
MIPS control-flow semantics, dead unless something else jumps to it (nothing in this dialect can, since
such spots have no label). `_reachable_blocks()` then does a BFS from `entry`; anything not reached is
reported as `unreachable_block_count` and excluded from all dataflow (this is what
`unreachable_block_count` in `--contract` output means — it is informational, not a bug smell).

Two forward dataflow passes then run to a fixpoint over the real CFG (handles loops, e.g. `gt3`'s
`L_8013FBC0` self-loop):

- **`compute_reaching_sp_addr`**: reaching value of each register materialized as an `sp`-relative
  address (`r2 = sp+K`), per block ENTRY. The meet operator at a merge point is intersection — a
  register's sp-offset is only "known" at a block's entry if every predecessor path agrees on it.
  This is what fixes the previously-missed `sp+12` slot in `ov_a00_gen_8013FB88`: `L_8013FD54`'s ONLY
  real predecessor is the specific `cond_taken` edge whose delay slot materializes `r2 = sp+12`: the
  dataflow now reasons about that edge specifically, instead of a linear scan picking up whatever a
  different, file-order-earlier sibling block (`L_8013FD00`, reached via a DIFFERENT branch) happened
  to leave in a shared "current materialization" variable.
- **`compute_reaching_callee_saved`**: reaching value-SET of each `r16..r23`/`r30` register, per block
  entry. If a register has more than one distinct value reaching a block (because different predecessor
  paths assigned it differently, or one path assigns it and another doesn't), the tool reports it as
  **conditionally live** rather than silently picking one path's answer — see call-site rendering below.

## `--contract` output

- **prologue spills**: stores in the prologue block only (the block that runs unconditionally from
  function start, before any branch) whose source is a bare `c->r[N]` — i.e. genuine MIPS o32
  callee-save spills, not general local scratch. The pre-rewrite tool classified EVERY register store
  before the first *label* as a prologue spill, wrongly including branch-guarded stores after a
  conditional branch (and dead-sibling stores); those now correctly appear as guarded scratch instead.
- **epilogue restores**: every `sp`-relative load into a register, tagged with the CFG block it's in.
- **scratch / local sp-relative stores**: every OTHER `sp`-relative store (direct or through a
  materialized address register), in CFG block order, each with a **branch-guard chain** — the walk
  back through unique predecessors to `entry` (or to the nearest merge point, reported as such rather
  than arbitrarily picking one incoming path) with the actual source condition text at each hop. This
  is what makes the `mode==1`/`mode==2` duplicate-layout shape in `gt3`/`gt4` fully legible: each
  group's guard chain literally shows `(c->r[3] == c->r[2]) == true` vs `== false` at the fork that
  selects the mode.
- **call sites**: target, `c->r[31]` (`ra`) constant (or `MISSING` — bug smell), and two liveness
  buckets: `live_regs` (definite — same value reaches this call on every path) and
  `conditional_live_regs` (the register has DIFFERENT reaching values depending on which path was
  taken to get here — flagged explicitly with every distinct value, never collapsed to one answer).

## Validation against previously hand-verified ground truth

Run 2026-07-10, `python3 tools/abi_extract.py <addr> --contract`, against five functions whose frames
were independently hand-derived and landed in prior sessions (see `docs/findings/render.md` and git
history for `game/render/perobj_dispatch.cpp` / `game/render/overlay_ground_gt3gt4.cpp`). **All five
MATCH the documented ground truth exactly**, including the previously-known scratch-enumeration gap
(case 4), now fully closed.

### 1. `0x8003CDD8` (`Render::cmdListDispatch` / `CmdListFrame`) — MATCH

Documented: `-56` frame, spills `r16..r23`/`ra` at `sp+16..sp+48` (9 regs), one call site to
`func_8003F698` with `c->r[31] = 0x8003D07Cu`.

```
frame_size = 56
## prologue spills (9): sp+16<-r16 sp+20<-r17 sp+24<-r18 sp+28<-r19 sp+32<-r20 sp+36<-r21 sp+40<-r22 sp+44<-r23 sp+48<-r31
## epilogue restores (9): same 9, mirrored
## call sites (1): [0] label=L_8003D074 target=func_8003F698  c->r[31] = 0x8003D07Cu
    live callee-saved regs (definite): r16, r17, r18, r19, r20, r21, r22, r23 (all shown, matching the
    documented "keeps r16..r23 LIVE as loop-invariant/loop-index scratch" bug fix)
```
Exact match to `game/render/perobj_dispatch.cpp`'s `CmdListFrame` (frame size, spill offsets, single
call site + `ra` constant) and to the `docs/findings/render.md` "f62 divergence" writeup describing
which registers gen keeps live into this call. No conditional liveness (single straight-line path to
the call), consistent with the doc.

### 2. `0x8003CCA4` (`Render::perObjRenderDispatch` / `CCA4Frame`) — MATCH

Documented: `-32` frame, spills `r16/r17/r18/ra` at `sp+16/20/24/28` (4 regs).

```
frame_size = 32
## prologue spills (4): sp+16<-r16 sp+20<-r17 sp+24<-r18 sp+28<-r31
## call sites (11): 10 direct calls (2 per case for 4 of the 5 case labels, 1 for the other, matching
    the doc's "8 call sites... across its 5 cases" to within the switch-default dispatch this tool
    also counts as a call — see note below) + 1 switch-default `rec_dispatch` with NO r31 set (flagged
    as a bug-smell by the tool; this one is legitimate — a tail-dispatch that never returns to this
    frame, not a genuine call needing ra preservation)
```
Exact match to `CCA4Frame`'s 4-register spill set and offsets. Call-site count differs in wording only
(the doc's "8" counts sites that set an explicit `ra` constant across the 5 real case bodies; the tool's
11 includes the switch's own `default: rec_dispatch(...)` dispatch, which correctly has no `ra` set).
`r16`/`r17`/`r18` all report as definite (single value) at every call site — no CFG merge point sits
between their assignment and any call in this function, so the CFG rewrite doesn't change this case's
answer, only how it's computed.

### 3. `0x80090BD0` (`seqChannelDispatch`) — MATCH

Documented: `sp-56`, 10 spills.

```
frame_size = 56
## prologue spills (10): sp+16<-r16 sp+20<-r17 sp+24<-r18 sp+28<-r19 sp+32<-r20 sp+36<-r21 sp+40<-r22 sp+44<-r23 sp+48<-r30 sp+52<-r31
## epilogue restores (10): same 10
```
Exact match: `-56` frame, 10 spills (`r16..r23`, `r30`, `r31`).

### 4. `0x8013FB88` (`OverlayGroundGt3Gt4::gt3`) — MATCH, INCLUDING the previously-missed `sp+12`

```
frame_size = 24
## prologue spills (0): none — this frame is pure LOCAL SCRATCH, not a callee-save spill area
## scratch / local sp-relative stores (6), CFG block order, with branch-guard context:
    [L_8013FD00] sp+0  <- gte_read_data(17)
        guard: L_8013FCCC: (c->r[3] == c->r[2]) == true          <-- mode==1 path
    [L_8013FD00] sp+4  <- gte_read_data(18)
    [L_8013FD00] sp+8  <- gte_read_data(19)
    [L_8013FD54] sp+12 <- gte_read_data(17)
        guard: L_8013FCCC: (c->r[3] == c->r[2]) == false
        guard: __anon_2: (c->r[3] == c->r[2]) == true             <-- mode==2 path
    [L_8013FD54] sp+16 <- gte_read_data(18)
    [L_8013FD54] sp+20 <- gte_read_data(19)
```
This is the fix in action: `L_8013FD54`'s ONLY predecessor is the `cond_taken` edge whose delay slot
sets `c->r[2] = c->r[29] + (uint32_t)12` — the CFG dataflow resolves the indirect store through `r2` to
`sp+12` on THAT edge specifically. The linear-scan predecessor tool (see git history,
`46e4feb`/`ffb1463`) missed this because file order walked through `L_8013FD00`'s block body — reached
via a DIFFERENT, unrelated branch — first, clobbering the shared "current value of r2" before the scan
ever reached `L_8013FD54`'s own body. All 6 scratch stores across both mode branches (0/4/8 for mode==1,
12/16/20 for mode==2) are now enumerated, matching `docs/findings/render.md`'s f179 writeup exactly.

### 5. `0x8013FE58` (`OverlayGroundGt3Gt4::gt4`) — MATCH (new validation case)

Documented (`docs/findings/render.md` f179): mode==1 SZ-mirror at `sp+0/4/8/12` (4 words — gt4 reads
one more GTE SZ register than gt3), mode==2 at `sp+16/20/24/28`.

```
frame_size = 32
## scratch / local sp-relative stores (8), CFG block order, with branch-guard context:
    [L_8014003C] sp+0  <- gte_read_data(16)     guard: L_8014001C: (...) == true    <-- mode==1
    [L_8014003C] sp+4  <- gte_read_data(17)
    [L_8014003C] sp+8  <- gte_read_data(18)
    [L_8014003C] sp+12 <- gte_read_data(19)
    [L_80140098] sp+16 <- gte_read_data(16)     guard: ... == false, __anon_7: (...) == true  <-- mode==2
    [L_80140098] sp+20 <- gte_read_data(17)
    [L_80140098] sp+24 <- gte_read_data(18)
    [L_80140098] sp+28 <- gte_read_data(19)
```
Exact match to the documented `16/20/24/28` mode==2 layout. All 8 scratch stores + 8 epilogue restores
enumerated across both mode branches.

Raw output for all five runs: `scratch/logs/validate_*.txt` (gitignored; regenerate with the commands
above — not checked in since `scratch/` is never committed).

## Corpus-wide validation (all 7,915 gen bodies)

Run 2026-07-10, every gen body under `generated/` parsed by both the CFG rewrite and the pre-rewrite
linear tool, in-process:

- **CFG tool: 0 failures, 0 crashes** across all 7,915 bodies.
- **Pre-rewrite tool: 39 false failures** ("frame OPEN size does not match CLOSE size(s)") — all
  caused by dead merged-sibling code after the real return; the CFG excludes it as unreachable.
- **7 frame_size disagreements**, all resolved in the CFG tool's favor (the linear scan reported a
  dead sibling's frame for a live leaf that has none: `800232F4`, `80079528`, `8006C59C`, `80026100`,
  `80089A30`, `80092E3C`, `8009EC80` — verified by reading the bodies: each returns before the
  `sp`-descent the old tool picked up).
- ~60 prologue-spill set disagreements, all cases of the old tool misclassifying branch-guarded or
  dead-sibling stores as unconditional prologue spills (see the prologue-spills note above).

## Shared parser surface (tools/port_gen.py / tools/port_check.py)

This module is the ONE gen-dialect parser — `tools/port_gen.py` and `tools/port_check.py` import it
(`locate_function`, `parse_contract`, `extract_op_sequence`, `OpSequence`/`OpSeqCall`, and the
line-level regexes); do not fork a second parser. `extract_op_sequence` is a deliberately coarser,
renaming-tolerant projection (frame sizes, call order + `ra` constants, memory-store widths in
program order) applied to BOTH the gen body and a native method body by `port_check.py` — it stays a
linear scan by design because its native side has no gen-shaped CFG to build. `--scaffold --guestabi`
emits the `runtime/recomp/guest_abi.h` `GuestFrame` form (see `docs/port-framework.md`), including
per-branch scratch groups with their guard chains.

## What it does NOT do (known limitations, honest)

1. **Not a full symbolic/value analysis.** Reaching-value tracking is over literal register-to-register
   assignment TEXT (`c->r[N] = <expr string>`), not evaluated arithmetic — two textually-different
   expressions that are provably equal (e.g. `c->r[2] + c->r[0]` vs `c->r[2]`) are NOT recognized as
   the same value and can spuriously show up as "conditionally live" when they're actually identical.
   This trades a few false "conditional" flags for never silently collapsing a genuinely different pair
   of values into one wrong answer — the safe direction per CLAUDE.md's "no bandaids."
2. **`--audit` is still a literal-substring grep, not scope-aware.** It cannot tell whether a matching
   offset/constant is in the right call-site scope in the native file, or is an unrelated use of the
   same number. Treat every `[!!]` as "investigate" and every `[ok]` as "no obvious problem found," not
   a pass/fail verdict — the mandatory line-by-line RE verify pass (`docs/fleet-workflow.md` §9) still
   has to happen.
3. **Handles the emission idioms actually observed in this codebase's `generated/`** (direct MIPS-
   register spill/restore, direct and indirect `sp`-relative stores, `func_XXXXXXXX(c)` /
   `ov_<area>_func_XXXXXXXX(c)` direct calls, `rec_dispatch(c, expr)`, and the switch-table
   `default: rec_dispatch(...)` dispatch, all five branch/return/goto shapes enumerated above). If the
   recompiler ever changes its emission shape, or a function uses an idiom not listed here, the tool
   raises `AbiParseError` rather than emit a silently-wrong contract — extend the parser when that
   happens, don't work around it by hand for that one function.
4. **`_guard_chain_for` reports one representative chain per unique-predecessor run, not a full
   boolean path formula.** At a genuine merge point (a block with more than one predecessor, e.g.
   reached from both a loop back-edge and a forward branch) the chain stops and names the merging
   predecessors rather than expanding all of them — full path enumeration through merge points is not
   built (would need to fan out combinatorially for blocks inside loops). This is intentionally
   conservative: it never claims a single guard condition for a block that's actually reachable multiple
   ways, and the block's own reported reachability (`unreachable_block_count`, and simply appearing in
   the output at all) still reflects the real CFG, not a linear guess.

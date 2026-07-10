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
  (the `mode==1`/`mode==2` SZ-scratch idiom in `ov_a00_gen_8013FB88`)

`tools/abi_extract.py` parses the RECOMPILED C TEXT under `generated/` (ground truth — never Ghidra's
pseudo-C, which garbles COP2/delay slots per CLAUDE.md's "Ghidra first, never disas.py walk" rule) for a
given guest address and emits the contract mechanically, so these stop being hand-derived from scratch
every time.

This is a **static text parser over the recompiler's fixed emission idioms**, not a general MIPS
dataflow/CFG engine. It fails loudly (`AbiParseError`, nonzero exit) rather than emit a guessed-wrong
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

## What it does NOT do

- **No CFG / dataflow.** It's a single linear pass over the function body in program-order (with MIPS
  branch-delay-slot compounds `{ int _t = (cond); DELAY_INSN; if (_t) goto L; }` decomposed into their
  real execution order). Register-liveness tracking for call-site "live callee-saved regs" and for the
  indirect `sp`-address-materialization idiom (`r2 = sp+K; mem_w32(r2+0, ...)`) is a heuristic that can
  under-report across a branch: a register set on one path and consumed after a DIFFERENT path's label
  can get spuriously invalidated by intervening statements that occur earlier in FILE order but never
  actually execute on the taken path. See the `ov_a00_gen_8013FB88` validation run below — it misses
  the `sp+12` scratch slot for exactly this reason (a real per-path CFG walk would fix it; not built,
  given the effort/complexity tradeoff — flag if it becomes a recurring gap).
- **`--audit` is intentionally noisy/heuristic** — literal-substring greps for the frame delta, spill
  offsets, and `ra` constants appearing SOMEWHERE in the native file. It cannot tell whether a match is
  in the right call-site scope, or whether a matching offset represents the actual spill vs. an
  unrelated use of the same number. It is a cheap first pass, not a verify pass — the mandatory
  line-by-line diff against `generated/` (`docs/fleet-workflow.md` §9) still has to happen.

## Validation against previously hand-verified ground truth

Run 2026-07-10, `python3 tools/abi_extract.py <addr> --contract`, against four functions whose frames
were independently hand-derived and landed in prior sessions (see `docs/findings/render.md` and
git history for `game/render/perobj_dispatch.cpp`). All four MATCH the documented ground truth exactly
on frame size and spill/restore set; see the "known limitation" note on the fourth.

### 1. `0x8003CDD8` (`Render::cmdListDispatch` / `CmdListFrame`) — MATCH

Documented: `-56` frame, spills `r16..r23`/`ra` at `sp+16..sp+48` (9 regs), one call site to
`func_8003F698` with `c->r[31] = 0x8003D07Cu`.

```
frame_size = 56
## prologue spills (9): sp+16<-r16 sp+20<-r17 sp+24<-r18 sp+28<-r19 sp+32<-r20 sp+36<-r21 sp+40<-r22 sp+44<-r23 sp+48<-r31
## epilogue restores (9): same 9, mirrored
## call sites (1): [0] label=L_8003D074 target=func_8003F698  c->r[31] = 0x8003D07Cu
    live callee-saved regs: r16, r17, r18, r19, r20, r21, r22, r23 (all shown, matching the
    documented "keeps r16..r23 LIVE as loop-invariant/loop-index scratch" bug fix)
```
Exact match to `game/render/perobj_dispatch.cpp`'s `CmdListFrame` (frame size, spill offsets, single
call site + `ra` constant) and to the `docs/findings/render.md` "f62 divergence" writeup describing
which registers gen keeps live into this call.

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

### 3. `0x80090BD0` (`seqChannelDispatch`) — MATCH

Documented: `sp-56`, 10 spills.

```
frame_size = 56
## prologue spills (10): sp+16<-r16 sp+20<-r17 sp+24<-r18 sp+28<-r19 sp+32<-r20 sp+36<-r21 sp+40<-r22 sp+44<-r23 sp+48<-r30 sp+52<-r31
## epilogue restores (10): same 10
```
Exact match: `-56` frame, 10 spills (`r16..r23`, `r30`, `r31`).

### 4. `0x8013FB88` (`OverlayGroundGt3Gt4::gt3`) — MATCH on frame, PARTIAL on scratch (documented gap)

```
frame_size = 24
## prologue spills (0): none — this frame is pure LOCAL SCRATCH, not a callee-save spill area
## scratch / local sp-relative stores (5), with branch-label context:
    [L_8013FD00] sp+0  <- gte_read_data(17)
    [L_8013FD00] sp+4  <- gte_read_data(18)
    [L_8013FD00] sp+8  <- gte_read_data(19)
    [L_8013FD54] sp+16 <- gte_read_data(18)
    [L_8013FD54] sp+20 <- gte_read_data(19)
```
Correctly shows the `-24` frame has NO register-save spills (matching gt3's real shape — the frame is
scratch, not callee-save) and correctly surfaces the TWO DIFFERENT per-branch scratch layouts
(`L_8013FD00` at offsets 0/4/8, `L_8013FD54` at offsets 16/20 — the mode-dependent-scratch shape this
tool exists to catch). **Known gap**: `L_8013FD54` should also show `sp+12 <- gte_read_data(17)`
(materialized via `r2 = sp+12` in a branch-delay slot several lines earlier, on the SAME predecessor
edge as the label); the linear scan's register-liveness tracking gets invalidated by intervening
statements from the OTHER (not-actually-taken-together) branch that appear earlier in file order. This
is the CFG-vs-linear-scan limitation documented above — flagged here rather than silently wrong.

Raw output for all four runs: `scratch/logs/validate_*.txt` (gitignored; regenerate with the commands
above — not checked in since `scratch/` is never committed).

## Known limitations (summary)

1. No CFG/dataflow — a purely linear, program-order scan. Cross-branch register liveness (both the
   callee-saved "live at call site" heuristic and the `sp`-address-materialization idiom) can miss or
   over-report across labels reached from multiple predecessors. Always cross-check the flagged call
   sites and scratch stores against `generated/` by eye for anything non-trivial.
2. `--audit` is a literal-substring grep, not scope-aware. Treat every `[!!]` as "investigate", and
   every `[ok]` as "no obvious problem found", not as a pass/fail verdict.
3. Handles the emission idioms actually observed in this codebase's `generated/` (direct MIPS-register
   spill/restore, direct and indirect `sp`-relative stores, `func_XXXXXXXX(c)` / `ov_<area>_func_
   XXXXXXXX(c)` direct calls, `rec_dispatch(c, expr)`, and switch-table `default: rec_dispatch(...)`
   dispatch). If the recompiler ever changes its emission shape, or a function uses an idiom not listed
   here, the tool raises `AbiParseError` rather than emit a silently-wrong contract — extend the parser
   when that happens, don't work around it by hand for that one function.

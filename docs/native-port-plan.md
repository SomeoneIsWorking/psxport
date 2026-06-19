# Top-down native port plan (boot → menu → game), 2026-06-19

## ★ DIRECTION LOCKED (user, 2026-06-19): HAND-WRITTEN NATIVE C++ for boot→first-cutscene
"Hand-native engine; gameplay doesn't matter; but ALL THE WAY from boot INTO the first cutscene, and
ANYTHING along the way, needs to be NATIVE, using C++ as the spine."
- The **779 functions on the boot→first-cutscene path** (scratch/trace/boot2cut.funcs) must each be
  **hand-written native C++** — NOT interpreted, NOT machine-recompiled. C++ calls C++ (the spine).
- **Gameplay beyond the cutscene "doesn't matter"** — per-entity AI/physics may stay interpreted/recomp.
- **The recompiler/substrate is NOT the product** for this path. The user explicitly rejected shipping
  recompiled (machine-translated) bodies. The recompiler stays ONLY as an RE *reference*: read
  generated/shard_*.c (now with jump-table recovery → accurate pseudo-C) + the MIPS to UNDERSTAND each
  function, then hand-write clean native C++. The PSXPORT_SUBSTRATE build stays opt-in dev scaffolding to
  keep the game runnable end-to-end while porting, but the SHIPPED boot→cutscene path uses hand-native C++
  only (tripwire == 0 interpreted AND no recompiled body invoked on the path).
- MECHANISM: each ported function = a hand-written native C++ override (rec_set_override). Un-ported
  functions interp TEMPORARILY; the tripwire (PSXPORT_INTERP_FUNCS) burns down to 0 as coverage grows.
  When 0 on boot→cutscene, the path is 100% native; then flip run.sh default to it.
- RECIPE per function: (1) read generated gen_func_<addr> + disasm (MAIN.EXE load 0x80010000) to get the
  algorithm; (2) write a clean native C++ function (use real types/structs where sensible, not blind
  c->r[] transcription) and register it via rec_set_override(addr, fn); (3) A/B verify vs the interp body
  (super-call) — diff RAM/regs/return on the real boot→cutscene run; (4) confirm the tripwire no longer
  lists it. Keep the game reaching the prologue at every step. Use subagents for batches (user-approved).
- ORDER: top-down from boot — crt0/init-prefix (ov_game_main's rc0 list) → START stage → DEMO menu
  (0x801062E4, 8-substate machine) → New Game → GAME prologue cutscene (0x8010637C). Engine/render/camera
  funcs already native (engine_submit etc.) count toward coverage.

## (superseded-as-product) earlier substrate investigation below — kept for the RE reference + tooling

User directive (2026-06-19, supersedes the "engine-native, gameplay-stays-recomp" framing where they
conflict): **make Tomba 2 fully PC-native, no black boxes, no unknowns. It must BEHAVE like a PC game
(native GTE math / native submit, not PSX emulation), built TOP-DOWN starting at boot.** RE/MIPS work is
deferred until the port *reaches* each subsystem (then disassemble + reimplement). Architecture target:
**C is the spine. C calls C; C calls the interpreter only for not-yet-ported leaves; the interpreter
NEVER calls back into C.** (Today it's inverted — see below.) Keep interp at the end only for things
like enemy AI, or remove it entirely.

## VERIFIED current state (do not trust "boot→menu is native" — it is NOT)
`runtime/recomp/native_boot.cpp` is a native C **frame loop + cooperative scheduler**, but it **runs the
game's recompiled MIPS through the interpreter** (`rc0`/`rec_dispatch`/`rec_coro_run`). So:
- The boot **driver** is native; the **init prefix** (native_boot.cpp ~276-312) and **all stage/menu
  logic** are *interpreted recompiled code*, NOT native C.
- **72 address-keyed overrides** exist; the interpreter jumps INTO them mid-run via `coro_native_call`
  (interp→C). On the boot→menu path, **16 of these fire** (e.g. `80050B08` game-main, `80080880`
  task-switch, `800846D0/F0` projection, thread/CD natives). That interp→C flipping is what the new
  architecture removes (by making the *caller* native instead).
- Measured (240-frame headless trace `scratch/trace/menu.trace`): **~379 unique guest funcs execute on
  boot→menu**, 369 MAIN.EXE-resident + 9 overlay. That is the boot→menu port surface.

## The menu = the DEMO stage (RE'd from a live menu RAM dump `scratch/bin/tomba2/ram_menu.bin`)
Stage sequence (native_boot per-frame log): START(`0x8010649C`) → **DEMO(`0x801062E4`)** = title/attract
/menu. task0 runs the DEMO sequencer as a coroutine (interpreted).

DEMO sequencer root **`0x801062E4`** is a clean **8-substate state machine**:
```
0x80106388 LOOP_HEAD:  v1 = task->[0x48] (substate); if (v1>=8) ...; jump table[v1]
  jump table @0x8010622C: s0=0x801063C0 s1=0x8010641C s2=0x80106464 s3=0x801064E8
                          s4=0x80106580 s5=0x801065DC s6=0x801065EC s7=0x80106668
  <substate body runs, advances task->[0x48]>
0x80106670 TAIL:  *0x1f800198 += 1 (frame ctr);  FUN_80051f80()  // YIELD (once/frame)
                  j LOOP_HEAD
```
task ptr = `*0x1f800138`; substate byte = `task+0x48`; per-stage frame counter = `*0x1f800198`.
Substate 0 body (`0x801063C0`): bumps +0x48 to 1, then calls `80045080, 80044bd4, 8007982C, 80075240,
8001CF00` — setup/load. **`FUN_80044bd4` busy-waits by YIELDING across frames** until a spawned loader
task sets a flag (see native_boot.cpp scheduler note) — so a substate body does NOT always complete in
one frame.

## THE architectural obstacle (decide before writing the native sequencer)
Cooperative yields today use **longjmp** (`ov_switch` → `longjmp(g_yield_jmp)` back to
`native_scheduler_step`'s setjmp). A native return-based frame-step does NOT compose with a longjmp that
unwinds past its C frame. To own the DEMO sequencer natively we must replace task0's longjmp-coroutine
with a **native return-based step**: each frame `demo_step(task0)` reads `+0x48`, runs that substate's
per-frame work, and RETURNS (== the yield). Multi-frame waits (`FUN_80044bd4`) become native poll-and-
return. This is the real "port the engine" work, not a transcription.

## Plan (top-down, each step keeps the game runnable + verified at the title)
1. **Native DEMO-stage dispatcher** owning the `+0x48` switch + yield/loop + frame counter, called
   DIRECTLY by `native_scheduler_step` for task0 when stage==DEMO (C→C, no interp dispatch). Each
   substate body starts as an interp leaf invoked so it runs to its natural per-frame stop. Verify the
   title still reaches/cycles identically (substate progression, frame counter).
2. **Port each substate body** (0..7) to native C, replacing its interp-leaf call. Resolve the yielding
   waits (e.g. `80044bd4`) into native poll-and-return. RE each leaf only as reached.
3. **Fold the 16 flip-point overrides into direct native calls** from the now-native callers (remove
   the interp→C flip on this path).
4. Repeat for START stage, then "Start Game" → the GAME stage / prologue. Then field.
5. **Behave like a PC game**: as ownership reaches the GTE/submit, move from PSX-fixed-point GTE +
   GP0-packet emulation toward native float transforms / native draw (the `proj_native_vertex` /
   native_dl groundwork already exists). No PSX quirks emulated where a native path is correct.

## SCOPE (measured 2026-06-19) + STRATEGY DECISION
Burn-down tripwire `PSXPORT_INTERP_FUNCS=<path>` (interp.cpp) records every UNIQUE guest fn the
interpreter runs (no override / not BIOS). Boot→first-cutscene (`PSXPORT_AUTO_NEWGAME=1`, prologue
GAME stage 0x8010637C reached frame 39): **779 unique interpreted functions** (671 resident MAIN.EXE,
107 runtime-loaded overlay, 1 low). List: `scratch/trace/boot2cut.funcs`.

Recompiler coverage: `generated/shard_*.c` has **1226 `gen_func_` bodies** (exact, compilable recompiled
C — delay slots, GTE ops, override table `g_override[]` + `func_XXXX` wrappers + address→fn switch in
`shard_disp.c`). BUT only **284 of the 779** have a body; **495 are reached only indirectly** (jalr/fn-
ptr/computed-jump) and were never statically discovered → not emitted. So linking shards alone covers
37% — the 495 need seeding into emit.py (it has a `seeds` list ~line 405) OR hand-porting.

**DECISION (faithful-first substrate, then hand-port engine to real native — the repo's recomp-port
methodology):** use recompiled bodies as a *temporary* no-interpreter substrate, then hand-port the
engine/stage/render layer to real native PC C on top, function-by-function, until the substrate is gone
from the boot→cutscene path. Recomp bodies are scaffold, NOT the end (user rejected "ship a blob").
Rationale: hand-retyping 779 funcs — many trivial leaves the recompiler already translated correctly
(e.g. 80082370 == libgpu GetTPage) — is absurd; the leaves have no meaningful "PC-native" form. The
PC-game character lives in the ENGINE layer (GTE projection, submit, camera, stage logic), which is
where the hand-port effort goes.

**BLOCKER — RESOLVED + VERIFIED (commit 6832af2, 2026-06-19):** shards used the PRE-OOP ABI; emit.py now
emits `void fn(Core* c)` + `c->mem_*(...)` and rec_decls.h includes core.h (which already declares the
free gte_op/gte_read_data/cpu_div/cop0/rec_dispatch helpers + OverrideFn). Regenerated; **all 8 shards +
shard_disp.c compile clean against the OOP Core** (verified: `c++ -std=c++17 -Iruntime/recomp -Igenerated
... -c generated/shard_*.c`). generated/ is gitignored (rebuilt by run.sh / emit.py). Remaining re-enable
steps: (1) DONE. (2) seed the 495 indirect addrs into
emit.py; (3) regen + compile shards + link (run.sh §4 currently SKIPS linking — see later-101); (4)
route `rec_dispatch`/`coro_native_call` to the recompiled `func_XXXX` switch for recompiled addrs, else
interp; (5) reconcile the 72 address-keyed runtime overrides with the index-keyed `g_override[]` table
(register runtime natives into g_override at the right index, or keep the address-keyed path as the
override layer ABOVE the recompiled switch). Overlays: emit.py already takes `--overlays`; watch address
aliasing (multiple overlays share 0x80106000+ — the live overlay set for boot→DEMO→GAME must be the
emitted one).

### ⚑ run.sh DEFAULT must be the NATIVE path, not the interpreter (USER, 2026-06-19)
Once the substrate is stable, flip the DEFAULT build (./run.sh + build_port.sh) to PSXPORT_SUBSTRATE
(native compiled bodies); the interpreter-only build becomes the OPT-OUT (A/B). Today substrate is opt-in
via PSXPORT_SUBSTRATE=1 only because it still derails (below). Do the flip after the derail is fixed.

## SUBSTRATE — ROOT CAUSE of the derail FOUND (2026-06-19): no jump-table recovery
**The recompiler has no in-function jump-table (switch) recovery.** A function with an internal computed
`jr` (switch on a bounded index) emits `rec_dispatch(c, reg)`; emit.py SEEDS the jump-table case labels
as fake "functions" (rec_func_index>=0). Under the substrate, the flat interp + compiled bodies dispatch
those labels as FUNCTION CALLS — but they are mid-function code with NO prologue/frame — so each "call"
corrupts the stack; after a few the return addr reads 0 → `jr ra` to 0 → derail (pc=0x10, ra=0, sp
underflowed past 0x80200000). CONFIRMED: ring buffer of compiled entries (PSXPORT_DEBUG=derail) shows the
derail inside the printf/format parser 0x8009A76C (case labels 8009AE60/8009AF5C) AND the sprintf/string
subsystem 0x80098xxx (80098D30/80098DB0...) — both jump-table-heavy. Excluding 0x8009A000+ from routing
still derails in 0x80098xxx, proving it is GENERAL, not one function.
**THE FIX (unblocks the whole substrate), pick one:**
  (A) PROPER + GENERAL: add in-function jump-table recovery to emit.py — detect the `sltiu idx,N; ... lw
      t,jt(base); jr t` switch idiom, recover the N targets from the .rodata jump table, emit a C `switch`
      (or computed-goto over `L_xxxx:` labels already emitted) so the computed jr stays INSIDE the compiled
      body (no rec_dispatch, no fake-function labels). Stop seeding jump-table labels as functions.
  (B) STOPGAP: native overrides for the format/string subsystem (printf/sprintf/vsprintf at 0x8009A76C +
      0x80098xxx) — replaces the jump-table-heavy code with native C. Narrower; other jump-table fns remain.
  Recommended: (A). Diagnostics already in tree: PSXPORT_DEBUG=derail (one-shot reporter + compiled-entry
  ring), PSXPORT_SUBSTRATE_LO/HI (range gate) in interp.cpp coro_native_call.

## SUBSTRATE WIRING — DONE end-to-end, builds+runs, ONE derail to fix (2026-06-19)
Implemented the A/B-gated substrate (`PSXPORT_SUBSTRATE=1` build):
- emit.py: renamed generated `rec_set_override`→`shard_set_override` (so dispatch.cpp owns a HYBRID
  rec_set_override: interp map + g_override[]).
- dispatch.cpp: `#ifndef PSXPORT_SUBSTRATE` keeps interp-only (rec_func_index=-1, rec_dispatch=interp
  router); `#else` rec_func_index+rec_dispatch come from shard_disp.c, rec_set_override is hybrid.
- interp.cpp `coro_native_call`: `if (rec_func_index(tgt) >= 0) { rec_dispatch(c, tgt); return 1; }` —
  recompiled targets run as COMPILED C from inside the flat interp (no-op when interp-only: index always -1).
- build_port.sh: `PSXPORT_SUBSTRATE=1` adds -DPSXPORT_SUBSTRATE -Igenerated + links generated/shard_*.c
  (compiled with `$CXX -x c++ -O1` — they're C++ content in .c files). Default build unchanged.
**Result:** substrate compiles + links + RUNS. Boot→cutscene interpreted-func count drops **779 → 223**
(the recompiled bodies now execute compiled). BUT it DERAILS in the init prefix (ov_game_main, after the
native projection setup `[geom] SetGeomScreen`): the flat interp ends up at insn=0xFFFFFFFF (a `jr ra`
to an unset/garbage return addr) and spins on `bad opcode`. Last tripwire entries before derail:
800865C0, 80091EA8 (both `<-DEAD0000` = top-level native_boot dispatch). The compiled↔interp call/return
contract via `rec_interp` (sets ra=CORO_SENTINEL, runs flat, restores) is correct in principle, so the
derail is EITHER a specific miscompiled shard body (the shards were "0-diff" pre-OOP; may have rotted) OR
a boundary case. DIAGNOSED (PSXPORT_DEBUG=derail one-shot reporter in interp_flat; PSXPORT_SUBSTRATE_LO/HI
range gate in coro_native_call): the derail is **pc=0x10, ra=0x00000000, sp=0x80200030** (sp UNDERFLOWED
past the 2MB top 0x80200000 — a function returned via `jr ra` with ra=0 and an over-popped stack). It
fires in the init prefix right AFTER `[geom] SetGeomScreen`, i.e. inside the projection-setup subtree
`gen_func_800509B4 → func_80050738 → func_80083B30(x2)/func_80083BF0(x2) → func_80086604`. ALL of those
recompiled bodies have CORRECT frames (verified by hand) — so the ra=0/stack-underflow comes from a
deeper or boundary case, NOT a jump table (gen_func_800509B4 has none). Range-bisection can't isolate
further because routing one entry pulls its whole compiled subtree (direct `func_X(c)` calls bypass the
gate). NEXT: add a ring buffer of entered compiled-function addrs (push in coro_native_call's
`rec_func_index>=0` branch) and dump it in the derail reporter → names the exact function returning to
ra=0. Suspect: a recompiled body whose o32 stacked-argument or $ra save/restore the compiler got wrong,
OR a compiled→BIOS/override boundary that doesn't preserve sp/ra. Logs: scratch/logs/derail.log,
scratch/logs/bis_*.log. Default interp-only build verified still good (rec_func_index==-1 there).

### EXECUTION ORDER (next session, full budget; subagents OK per user)
1. Re-enable the recomp substrate (steps 1-5 above). Verify: tripwire count drops sharply, game still
   reaches the prologue (`[autonewgame] reached GAME`), no behavior change. This kills the interpreter
   for the 284 (+ newly-seeded) resident funcs.
2. Seed + emit the 495 indirect funcs; re-verify tripwire → near-zero resident interpreter.
3. Overlay stage code (DEMO 0x801062E4 menu + GAME 0x8010637C prologue): emit + link the live overlay,
   OR hand-port these (they're the menu/cutscene logic — prime "real native" targets). DEMO root is the
   8-substate machine above; port its sequencer to a native return-based frame-step (resolve the longjmp
   yield → native poll-and-return).
4. Tripwire == 0 boot→cutscene ⇒ goal met (no interpreter). Then begin replacing recompiled bodies with
   real native PC engine C, top-down (DEMO/GAME stage → render → camera), moving GTE/submit to native
   float/PC-draw where it isn't already.

Per-function port+verify recipe (for hand-ported engine fns): write native override (read gen_func body
+ MIPS as RE ref), build (`tools/build_port.sh <file>`), A/B vs the recomp/interp body
(`PSXPORT_<X>_RECOMP` or the override's super-call), diff RAM/regs on the real path; the tripwire confirms
the fn no longer hits interp. Keep the game reaching the prologue at every step.

## Tooling notes
- Disassemble the live overlay from a menu RAM dump (overlays aren't in MAIN.EXE at a fixed addr):
  `scratch/bin/tomba2/ram_menu.bin` (offset = addr & 0x1FFFFF). MAIN.EXE-resident funcs disasm from
  `scratch/bin/tomba2/MAIN.EXE` (load 0x80010000, 0x800 header).
- Reach the menu headless: `PSXPORT_REPL=1` + piped `run N / dumpram / stage / quit`, disc via
  `PSXPORT_TOMBA2_DISC`. Trace interpreted calls: `PSXPORT_INTERP_TRACE=<path>` (jal/jalr firehose).
- The matrix-stride fix in `engine/engine_submit.cpp` (`submit_perobj_flush`, col*2 not col*1) is RE-
  confirmed from `gen_func_8003CDD8` MIPS — kept regardless of the explosion (genuine porting bug).
</content>

# Top-down native port plan (boot → menu → game), 2026-06-19

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

## Tooling notes
- Disassemble the live overlay from a menu RAM dump (overlays aren't in MAIN.EXE at a fixed addr):
  `scratch/bin/tomba2/ram_menu.bin` (offset = addr & 0x1FFFFF). MAIN.EXE-resident funcs disasm from
  `scratch/bin/tomba2/MAIN.EXE` (load 0x80010000, 0x800 header).
- Reach the menu headless: `PSXPORT_REPL=1` + piped `run N / dumpram / stage / quit`, disc via
  `PSXPORT_TOMBA2_DISC`. Trace interpreted calls: `PSXPORT_INTERP_TRACE=<path>` (jal/jalr firehose).
- The matrix-stride fix in `engine/engine_submit.cpp` (`submit_perobj_flush`, col*2 not col*1) is RE-
  confirmed from `gen_func_8003CDD8` MIPS — kept regardless of the explosion (genuine porting bug).
</content>

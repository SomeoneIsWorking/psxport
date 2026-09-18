# Lightrec delay-slot defects reach every port, and only a differential replay finds them

A miscompile in the shared dynarec is invisible to unit tests, to a frame-difference count, and to
a play-through: the game keeps running, one register is wrong, and the damage surfaces far from its
cause. The method below is the transferable part; the entry after it is what the method found.

## The method: replay the real routine from real RAM, JIT against interpreter

1. Dump the product's full 2 MB main RAM at the checkpoint where the oracle comparator first
   disagrees (Spyro: `dumpram` over the REPL at the first `GS_Playing` frame).
2. Build a small driver that links Lightrec directly, maps that RAM image verbatim, sets the guest
   registers the call site passes, plants a `syscall` as the return trap, and runs the routine.
3. Run it twice from the same image: `lightrec_execute` and `lightrec_run_interpreter`. Install the
   product's own `block_boundary` and `fallback_admission` callbacks, because they change which
   optimizations apply — `lightrec_local_branches` is disabled entirely when `block_boundary` is
   installed, so the product has no local branches and a defect hidden behind that flag only
   reproduces with the callback present.
4. Compare block by block, not once at the end: step each state with a one-cycle budget and diff all
   32 GPRs, the next PC, and RAM after every block. The first disagreeing block is the miscompiled
   one, and the register it disagrees on names the instruction.
5. Delta-debug the optimizer flags. Each `OPT_*` build is seconds, so bisecting the eleven flags is
   faster than reading the emitter.

A run that only compares the final answer will call an infinite loop "a different result" and tell
you nothing about where it started.

## 2026-09-18: a NOP'd branch kept its delay slot's deferred load

`lightrec_transform_ops` replaces a never-taken branch with a NOP through two paths. The three
constant-folding arms clear `LIGHTREC_LOAD_DELAY` on the following opcode first, because a
delay-slot load's write goes to `REG_TEMP` and is only committed by the branch's end-of-block
handler. The generic `is_nop()` path did not, so the load's target register was never written and
every later reader in the block saw the pre-load value.

A known-zero operand reaches that generic path routinely: `lightrec_patch_known_zero` rewrites
`BGTZ $rs` into `BGTZ $zero`, which `is_nop()` accepts. Spyro the Dragon `SCUS_942.28` hit it at
`0x8004DFAC`, where `bgtz $s6` carries `lw $a0, 0x2C($a0)` in its delay slot: `$a0` stayed at
`&g_Environment` instead of the collision header it loads, so the occlusion search walked the wrong
table and never terminated. The symptom the comparator saw was one wrong byte, the camera's
occlusion group, three thousand frames into a route.

Fixed in `shared/lightrec` `3fddb23`, which also collapses the four duplicated nulling sequences
into one `nullify_branch` owner so the correction cannot be missed again. Its regression test is
differential and was checked both ways: it reports `$a0 = 0x80000040` (the pre-load address) on the
unfixed optimizer and the loaded value on the fixed one.

The fixture that reproduces a defect like this comes from the guest shape that failed, not from the
emitter's point of view. The first attempt here used a local branch, whose target did not read the
loaded register, so `lightrec_handle_load_delays` never set the flag and the test passed on the
unfixed build. A regression test that has not been run against the broken code proves nothing.

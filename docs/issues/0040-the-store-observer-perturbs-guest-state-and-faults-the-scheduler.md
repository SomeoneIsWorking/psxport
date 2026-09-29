# The store observer perturbs guest register state, and on Mega Man X4 that faults the scheduler

**Status:** open. Instrument defect, root cause localised to the observed effect, the earlier
root-cause hypothesis **withdrawn and reverted**.

## THE CORRECTION FIRST, because this file previously said something false

An earlier revision of this issue claimed the observer **segfaults the JIT** on a delay-slot store,
and named a mechanism in Lightrec's `rec_b`: a branch-mode register-cache backup that the
observer's `regcache_reset` invalidates, restored by `leave_branch`. That was implemented
(`shared/lightrec` `8611c9c5`), built, and **it changed nothing** — the same two store PCs still
abort. The change has been **reverted** (`shared/lightrec` `e1a6a09`) and psxport's lightrec pin
restored. A change to a shared emitter's branch contract must not ship on a premise that a test
refutes, and this one did.

**The claim was wrong because the crash was misread.** I matched `libc_start_main` in the output to
detect a fault, and that substring appears in **every** backtrace, including a clean `abort()`. So
"aborted=1" was really measuring *the process died by any means*, and I read it as a JIT fault. The
backtrace says otherwise:

    libc.so.6(abort+0x26)
    megamanx4_port(x4::guest::callWithoutKnownReturn(Core*, unsigned int)+0x397)
    megamanx4_port(x4::frame::X4FrameDriver::stepFrame(Core&, unsigned int)+0x2ad)
    [watchdog] signal = 06                       <- SIGABRT, not SIGSEGV

**The aborting frame is Mega Man X4's own guest-call guard, not generated code.**

## What actually happens

The log line immediately before the abort is the whole finding:

    [x4-guest:error] guest call 0x80012600 exited fault at 0x80012710 after 56 cycles,
                      and this owner has no return point for it

`0x80012710` is `lw $v0, ($s0)` — and it is the **target of the scheduler's own `j`** at
`0x80012658` and `0x80012674`, both of which read `0x080049C4` at runtime. So with the observer
armed, the scheduler branches to its own next block and the load faults: **`$s0` is not what the
emitter expected.**

That is consistent with everything already measured about this instrument and nothing else:
`0039` established that arming it calls `lightrec_invalidate_all` and `lightrec_free_all_blocks`,
instruments **every** store in **every** block, and that the hook calls `lightrec_clean_regs` and
`lightrec_regcache_reset`. An instrument that invalidates all blocks and resets the register cache
mid-emission is not a passive tap, and **this is the first measurement showing it changing a guest-
visible value.**

## The control matrix, and what the discriminator really is

Mega Man X4, headless, 400 frames, `PSXPORT_DEBUG=store-observe`, one store PC per run:

| armed store PC | instruction | events | outcome |
|---|---|---|---|
| `0x80012628` | `sw $v0, -0x7d00($at)` | 400 | clean |
| `0x80012724` | `sw $v0, ($s0)` — delay slot of `beqz` at `0x80012720` | 0 | abort |
| `0x800126A8` | `sh $s1, ($v0)` — delay slot of `jal 0x800EDdbc` at `0x800126A4` | 0 | abort |
| `0x80012628` | repeated | 400 | clean |

**The honest reading of the discriminator:** all three failing PCs are delay slots, and the passing
one is not — so "delay slot" is still the best available predictor. **But it is a correlation
observed over three PCs, not a mechanism.** The stale-backup hypothesis explained that correlation
and was wrong, so whatever the real cause is, it is not yet known. A fourth data point that broke
the pattern would be worth more than another restatement of this one.

The positive control stands and is unaffected: an ordinary store yields **400 events in 400 frames,
twice, with no fault** — one per vblank, which is the expected count for the scheduler's cursor
store. The instrument is not broken in general; it is *disruptive* in a way that reaches guest state.

## The trap worth keeping, because it will recur

The failing runs report **0 events**. Zero is the truthful reading — the counter never reached a
value — but a reader scanning "did the observer see anything" reads it as a clean negative
measurement of the guest. **It is not: the guest faulted and the port aborted before any report.**
Three separate readings in this investigation were wrong in exactly this way — a `MATCHED NONE`
from a tap nothing writes, `is3d` reading zero because its feeders were deleted, and now a
register-file dump whose `pc` field is the *next* block. **A zero produced by a process that died
is not a measurement, and the exit status and the teardown report both have to be read with it.**

## Named next steps

1. **Establish the discriminator with a negative case.** Find a store instruction that is a delay
   slot and whose arming does *not* fault, or an ordinary store that does. Until one exists, the
   delay-slot correlation rests on three points.
2. **Read what the observer does to `$s0` at the scheduler's block boundary.** The fault is a load
   of `$s0` immediately after a `j`; the question is whether the hook's `regcache_reset` at
   `0x80012628`-time blocks changed which native register `$s0` is bound to, and the emitted code
   for the *next* block was compiled against the wrong binding.
3. **Decide the instrument's status explicitly.** A diagnostic that can fault the guest is not
   usable for attribution on this port, and the port should say so at the point of use rather than
   leave a future reader to rediscover that a zero may mean "crashed".

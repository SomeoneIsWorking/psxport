# Tomba! 2 (USA, SCUS-944.54) — PSX kernel thread model & top-level control flow

Goal: own the game's control flow — understand its kernel thread model and find the
top-level "game-mode" dispatcher, so we can deterministically know what the game is
doing instead of sampling the wrong thread with pixel/PC heuristics.

All addresses/values below are from real runs under `runtime/wide60rt` on the OpenBIOS
HLE (`bios/scph5501.bin` = OpenBIOS). Tools written for this lived under
`scratch/agent-thr/` (`tcb.py` TCB parser, `stackwalk.py`, `mdis.py` skipdata disasm).
Dumps: `scratch/bin/tomba2/state/{title,loading,demo}.bin` +
`scratch/agent-thr/dumps/{f900,f1200,f1600,f3120}.bin`.

---

## 1. Kernel TCB (thread control block) array — layout & how to read it

OpenBIOS uses the PCSX-Redux kernel. The kernel work area is the classic A-table at
`0x80000100`. Layout derived from `vendor/openbios-src` and **verified against
`title.bin`** (`__globals` @ `0x80000100` matches the `/* 100 */` field comments and the
`.base` section in `src/mips/openbios/psx-bios.ld`):

```
__globals @ 0x80000100   (struct in src/mips/openbios/kernel/globals.h)
  +0x08 processes ptr        title.bin: 0x80008F58   (process[0].thread = current thread)
  +0x10 threads  ptr         title.bin: 0x80008F68   <-- TCB ARRAY BASE
  +0x14 threadBlockSize      title.bin: 0x300 = 4 slots * 0xC0
  +0x20 events   ptr         0x80008D90 (size 0x1C0)
```

There are **exactly 4 TCB slots** (`threadBlockSize 0x300 / sizeof(Thread) 0xC0 = 4`).
The "current thread" = `*(process[0].thread)` = `*(*(0x80000108))`.

### struct Thread (stride 0xC0 = 192 bytes)
From `src/mips/common/kernel/threads.h`:

```
+0x00  flags        0x1000 = FREE slot, 0x4000 = USED/active
+0x04  flags2
+0x08  GPR[0..31]   r0,at,v0,v1,a0,a1,a2,a3, t0..t7, s0..s7, t8,t9,k0,k1, gp,sp,fp,ra
          gp=r28 -> +0x78   sp=r29 -> +0x7C   fp=r30 -> +0x80   ra=r31 -> +0x84
+0x88  returnPC     (saved PC)
+0x8C  hi
+0x90  lo
+0x94  SR
+0x98  Cause
+0x9C  unknown[9]   (to +0xC0)
```

### Reading a thread's saved PC/SP/regs
For TCB slot i:  `base = *(0x80000110)`,  `tcb = base + i*0xC0`.
- saved PC = `*(tcb+0x88)`,  SP = `*(tcb+0x7C)`,  RA = `*(tcb+0x84)`, GP = `*(tcb+0x78)`.

**Gotcha (verified):** for a *descheduled* thread, `returnPC (+0x88)` is parked at
`0xBFC068D0` (a kseg1 BIOS-ROM yield/return stub). The meaningful "where will it
resume" pointer is then the saved **RA (+0x84)**, not PC. Only the currently-running
thread has a live game-code PC in +0x88. (This is exactly the kind of thing that makes
PC sampling lie.) `scratch/agent-thr/tcb.py` prints both PC and RA per slot with region
tags.

---

## 2. Live thread enumeration per state

Parsed with `tcb.py`. Region key: StrPlayer 0x80082xxx-0x8009Axxx; resident game
0x80010000-0x80038800; overlay/player 0x80044xxx-0x80052xxx; StrPlayer-pace 0x80050CExx.

### Title screen (`title.bin`)
```
slot0  USED  CURRENT  PC=0x80050CE8  [StrPlayer per-frame pace spin]   sp=0x801FFFB8
slot1  USED           PC=0xBFC068D0  RA=0x80051FA4 [StrPlayer scheduler] sp=0x801FE9B8
slot2  FREE
slot3  FREE
```

### "Loading....." dwell (`loading.bin`)
```
slot0  USED  CURRENT  PC=0x80050CE8  [StrPlayer pace spin]
slot1  USED           PC=0xBFC068D0  RA=0x80051FA4 [StrPlayer scheduler]
slot2  USED           PC=0xBFC068D0  RA=0x80051FA4 [StrPlayer scheduler]  (3rd transiently)
slot3  FREE
```

### Attract-demo (`demo.bin`, and re-confirmed at f1200/f1600/f2000)
```
slot0  USED           PC=0xBFC068D0  RA=0x80051EE8 [StrPlayer scheduler]
slot1  USED  CURRENT  PC=0x8009CEC4  RA=0x801075BC [StrPlayer worker]
slot2/3 FREE  (at f1200/f1600 only slot0+slot1 exist)
```

**Conclusion: at the front-end (intro/title/loading/attract) there are only ever 2–3
kernel threads, and they are ALL the StrPlayer subsystem.** ChangeThread (B0 0x10) just
ping-pongs FF000000<->FF000001 every frame (callers 0x80051FA4 / 0x80051EE8). There is
**no separate "game logic" kernel thread running during the front-end** — the attract
demo is *streamed video/audio played by StrPlayer* (CD irq1 streams every frame; the
object-cull chokepoint 0x8007712C never fires through f2000).

### StrPlayer userland coroutine layer (the real "threads")
The two kernel threads host a **userland coroutine scheduler** (StrPlayer overlay
0x80051E80–0x80052010). Its task array is at **0x801FE000, stride 0x70**, fields:
`+0x00 state, +0x04 ctx/threadId, +0x0C resumePC, +0x10 gp`. The scheduler primitives
0x80080860/70/80 are thin wrappers around kernel **B0(0x0E) OpenThread / B0(0x0F)
CloseThread / B0(0x10) ChangeThread**; 0x80080890/0x800808A0 = Enter/ExitCriticalSection.
- task0: resumePC 0x801062E4 (StrPlayer playback)
- task1: resumePC **0x80044F58 (title) -> 0x800452C0 (loading/attract)** — the player
  *control* task; its entry function is re-set per phase. Verified live:
  `PSXPORT_WATCHW=0x801FE07C` shows the scheduler at 0x80051F3C write resumePC 0x800452C0
  into task1 at **frame 820** (the title->loading transition) = task1 is re-created with a
  new entry when the mode changes.

So "which thread is the game doing X on" at the front-end = a StrPlayer **coroutine**, not
a kernel thread. Read the coroutine state at 0x801FE000 (stride 0x70), not just the 4 TCBs.

---

## 3. The top-level game-mode dispatcher

There are TWO distinct sequencers; do not conflate them:

### (a) Player-clip sequencer (front-end video chain) — NOT the mode machine
The StrPlayer control task (task1, 0x80044xxx–0x80045xxx) calls resident helper
**0x8001DC40 -> 0x8001D940** to spawn/advance the next streamed clip, writing a per-clip
state to **0x800BE0E4** (writer 0x8001D95C, ra=0x8001DC88). This fires at every clip
boundary (f820, f888, f936, f1006, f1100…). It sequences title->loading->attract *video*,
but it is just clip plumbing, not the game's mode state machine.

### (b) The actual top-level mode dispatcher — function **0x8001DC9C**
This is a 12-case `switch(state)` jump-table dispatcher (the game's mode/state machine):

```asm
8001DCD4  lbu   $v1, 2($s1)        ; v1 = MODE byte = *(stateObject + 2)
8001DCDC  sltiu $v0, $v1, 0xc      ; 12 cases (0..11)
8001DCE0  beqz  $v0, 0x8001e838    ; default / out-of-range
8001DCE8  lui   $v0, 0x8001
8001DCEC  addiu $v0, $v0, 0xd8     ; jump-table base = 0x800100D8
8001DCF0  sll   $v1, $v1, 2        ; index = mode*4
8001DCF4  addu  $v1, $v1, $v0
8001DCF8  lw    $v0, ($v1)         ; handler = table[mode]
8001DD00  jr    $v0                ; dispatch
```

**Mode jump table @ 0x800100D8 (12 entries, verified from RAM):**
```
 0 -> 0x8001E06C    6 -> 0x8001E434
 1 -> 0x8001DD94    7 -> 0x8001E5B0
 2 -> 0x8001E0FC    8 -> 0x8001E68C
 3 -> 0x8001DE1C    9 -> 0x8001E760
 4 -> 0x8001E2D4   10 -> 0x8001DD08
 5 -> 0x8001E35C   11 -> 0x8001DFF4
```

- **Mode variable = the byte at `stateObject + 2`** (a1/$s1 = the state object passed in).
- The dispatcher is invoked as an **object update method**: caller chain
  0x80021A24 (object-update loop) -> 0x80021A70 -> 0x8001DC9C, with $s1 = object*,
  $s2 = parent. The same loop routes flags-bit-8 objects to 0x8001E860 instead. This is
  the game's generic per-object update (same object system as the gameplay entity pool,
  base ~0x800EF478 stride 0xC4 documented in patches/tomba2/objects.md — the mode
  controller is one such object whose update fn is the 12-case machine).
- This dispatcher is **event-driven, not per-frame**: traced over f0–f2000 it fires
  exactly ONCE (a single hit in that window, at a scene boundary), and is otherwise
  dormant while StrPlayer owns the screen. (The per-clip writer 0x8001D95C/0x800BE0E4
  fires more often — f820/f888/f936/f1006/f1100 — but that is the clip plumbing in 3a,
  not this mode machine.) It is the mode machine you want to own to know
  intro->title->attract->newgame;
  it ticks at scene transitions, advancing the mode byte and (via case handlers ->
  0x8001DC40) spawning the appropriate StrPlayer clip task for the new mode.

### How to OWN the control flow (recommended levers)
1. **Mode = `*(stateObject+2)`**, dispatched at 0x8001DC9C via table 0x800100D8.
   Hook 0x8001DC9C (a1 = stateObject) to capture the live object ptr, then
   `PSXPORT_WATCHW` on `stateObject+2` (note: +2 is a byte; the watch tool logs the
   aligned word `stateObject&~3`) to observe every mode change deterministically.
2. The front-end clip state lives separately at **0x800BE0E4** (per-clip), written by
   0x8001D95C; watch it to follow the title/loading/attract *video* progression.
3. Coroutine state: read 0x801FE000 (stride 0x70, +0x0C = resumePC) to know what each
   StrPlayer task is doing; task1's resumePC switching 0x80044F58->0x800452C0 marks the
   title->load transition.

---

## Dead ends / falsified (do not re-chase)
- **PC histogram / `prof`** at the front-end is ~90% the StrPlayer pace spin
  (0x80050CE4–0x80050CF4). It tells you nothing about the mode — it samples the spinning
  StrPlayer kernel thread. This is the heuristic that kept failing.
- **`bt` stack-scan is unreliable here.** Thread stacks live near 0x801FFxxx and overlap
  StrPlayer's region; the scan reports many stale words. Specifically the "clean main
  chain" 0x80018DF8/0x80019C00/0x8001A188 it surfaced is **data, not return addresses** —
  0x80018DF8 is inside a pointer table (entries like {0xNN, 0x800NNNNN}). Confirm any
  "driver" by *tracing* it, never by trusting a stack-scan address.
- **0x80018B6C "entry"** (CLAUDE.md) disassembles to garbage (`teq zero,zero,2`) — it is
  not a code entry point in these dumps; treat with suspicion.
- **0x800675CC** (intro scene-phase from tomba2-scene-orchestrator.md) is **never written**
  during title->loading (WATCHW: 0 writes f0–f1000). It was the *intro-logo* phase var,
  not the front-end/title mode var. **0x8002C97C scene_update never fires** at the title
  either (0 hits / 30 frames).
- **Object-cull chokepoint 0x8007712C and render heartbeat 0x8003CCA4**: 0 hits through
  f2000 — the gameplay engine is genuinely idle; attract is video. To exercise the
  12-case mode machine in steady state you need a savestate INSIDE real gameplay (only
  `title.sav` exists today).

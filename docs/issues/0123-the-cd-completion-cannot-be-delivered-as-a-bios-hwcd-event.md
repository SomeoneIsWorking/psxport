---
id: 123
title: The CD-ROM completion cannot be delivered as a BIOS HwCD event, because Spider-Man 1 never opens one — and the framework's own promise in pumpStream rests on a delivery that does not happen
symptom: 14,000 black frames. `I_STAT & I_MASK = 0x004` (IRQ2) pending once and unclaimed, then the
         guest spins on I_MASK for the rest of the run. `Cd::pumpStream` declines to pump on the
         strength of a `irqPoll` delivery of the CD ready callback that no code performs.
state_items: S004,S013,S018,S019
tags: cd,bios,openevent,event-class,interrupts,root-cause,dead-end,s018
created: 2026-09-28
updated: 2026-09-28
---

## Answer

**The requested fix cannot work, and this is established from the guest's own bytes rather than
inferred. The `memcard` pair precedent does not transfer to the CD-ROM, because a delivery is
reachable only for an event class the guest actually OPENED, and Spider-Man 1 never opens
`0xF0000003`.**

`Hle::deliverEvent` marks a slot when

```cpp
ev[i].open && ev[i].enabled && ev[i].ev_class == evClass && (ev[i].spec & spec)
```

and a slot's `ev_class` is set in exactly one place — the `B0:0x08` OpenEvent arm of
`Hle::dispatchBios` (`runtime/psx/hle.cpp:540-566`). So "can class C be delivered to" is decided
entirely by what the guest opened, and **not at all** by which spec a caller picks. There is no spec
that reaches an absent slot.

**So the spec question has a harder answer than "unknown": the class has no subscriber either.**

## 1. The measurement, with its denominator

Over one 2,324-presented-frame run of the real disc, the product's own `ev` channel logged **9**
`OpenEvent` calls across **3** classes:

| class | count | specs |
|---|---|---|
| `0xF4000001` SwCARD | 4 | `0x0004`, `0x8000`, `0x0100`, `0x2000` |
| `0xF0000011` HwCARD | 4 | `0x0004`, `0x8000`, `0x0100`, `0x2000` |
| `0xF0000009` HwSPU | 1 | `0x0020` |
| **`0xF0000003` HwCD** | **0** | — |

The channel demonstrably fires (9 lines), so this is "scanned 9, matched 0" and not "instrument
never ran".

**This also confirms the precedent itself is sound**: `0xF0000011` is opened with spec `0x0004`,
which is exactly what `memcard.cpp:406` delivers. The card pair works because the class is open.

## 2. Independently, from the image's bytes

SLUS_008.75 rebuilt from `scratch/assets/spiderman1/SLUS_008.75` via `tools/redump_ram.py` (2 MB RAM
image, text `0xB6800` at `0x80010000`; byte-identical to the pre-existing `scratch/bin/spiderman/ram.bin`).

There are exactly **9** `lui $r,0xF000` sites in the whole text segment. Constant propagation over
basic blocks puts `0xF0000003` in a register at only **3** of them, and all three are the bodies
`CdInit` installs into the libcd callback slots:

```
0x8008A238  a0 = 0xF0000003 ; jal 0x8008F9D0 (B0:0x07 DeliverEvent) ; a1 = 0x20
0x8008A260  a0 = 0xF0000003 ; jal 0x8008F9D0 (B0:0x07 DeliverEvent) ; a1 = 0x40
0x8008A288  a0 = 0xF0000003 ; jal 0x8008F9D0 (B0:0x07 DeliverEvent) ; a1 = 0x40
```

Those are **`DeliverEvent` CALLERS, not `OpenEvent` registrations** — the distinction the whole
question turns on. The other six sites are HwCARD (×4, all OpenEvent via `0x800848E0`), SwCARD, and
HwSPU. `0x20` and `0x40` are the guest's own HwCD specs, read from its bytes; they are correct and
still useless, because no slot of that class is open to receive them.

## 3. What the guest actually does with the CD-ROM, which is not a BIOS event at all

The guest's own CD-ROM service routine is `0x8008C3E0` (its own prologue; four callers). It reads the
controller's interrupt-flag register and dispatches on the response type:

```
0x8008C400  lui $a0,0x800B ; lw $a0,0x3DE4($a0)   ; $a0 = [0x800B3DE4] = 0x1F801803
0x8008C40C  lbu $v0,($a0)                          ; read the pending response type
0x8008C414  andi $v0,$v0,7
0x8008C424  beqz $v0,0x8008C92C                    ; type 0 -> return, nothing pending
...
0x8008C614  addiu $v1,$v0,-1 ; sltiu $v0,$v1,5     ; types 1..5
0x8008C634  lw $v0,0x6670($at) ; jr $v0            ; jump table at 0x80096670
```

and where the type warrants a ready/sync notification it calls the registered function pointer
directly, with the staged status byte as `$a0`:

```
0x8008CACC  lw $v0,0x3B18($v0)   ; [0x800B3B18] = the CdReadyCallback slot
0x8008CADC  lbu $a0,($s5)        ; the status byte
0x8008CAE4  jalr $v0             ; CALL the guest's own callback
```

`0x800B3DDC` and `0x800B3DE4` hold `0x1F801801` and `0x1F801803` — the response FIFO and the
interrupt-flag register, both of which the framework's `CdcState` already models. The pointer slot
`0x800B3B18` is what `GuestCdStreamCallbackLayout::readyCallbackPointer` names and what
`cd_ready_callback_pointer()` already reads.

**So the consumer of a CD completion here is a guest CALLBACK POINTER, not a BIOS event class.**
That is the generic gap, and it is a different one from the one this issue was opened for.

## 4. Why the `GuestInterrupt` branch is nonetheless a real, separate defect

`cd_override.cpp:866-874` declines to host-pump on the strength of this comment:

> "irqPoll delivers it at a safe boundary after this native call returns"

`Hle::irqPoll` (`hle_interrupt.cpp:86-252`) services DMA callbacks, then offers `i_stat & i_mask` to
the SysEnq chain, then routes to the custom exception exit. **It never reads a CD ready-callback
slot.** So the promise is unkept — but fulfilling it as an *event* delivery is impossible per §1, and
fulfilling it as a *callback* delivery is a different design (§5).

`GuestCdStreamCallbackLayout::DeliveryOwner::GuestInterrupt` is therefore a declared and
unimplemented seam, and it is the right seam: the title's declaration is **correct** and the
framework does not honour it.

## 5. What the fix actually is, and why it is not a two-line event arm

A CD completion must be delivered by **invoking the guest's registered ready-callback pointer** at the
interrupt, with the response type as `$a0` — the `memcard` pair's *shape* (a library-level
notification then a BIOS-level one) is the precedent, but the *mechanism* is a callback dispatch, not
`deliverEvent`. Reading the slot's CURRENT value each time is required: libstr replaces `0x800B3B18`
with `0x800860B4` during a stream.

This is stated as the shape the work should take. **It is not implemented here**, because it is a
behavioural change to a framework the black-frames frontier has not yet been re-measured against,
and shipping it as a silent new CD path on the strength of a static reading is exactly the "plausible
fix" this issue exists to prevent.

**No double-delivery risk is created by this issue, because no delivery path was added.** When the
path is implemented, `DeliveryOwner` is the gate and only `spider1` selects `GuestInterrupt`; the
`HostPump` case must be shown to still get exactly one callback.

## 6. The test, and the discriminator that proved it can fail

`tests/test_cd_hwcd_event_subscriber.cpp` (registered by the `tests/` glob; 175/175 green).

The positive control is the point: a test that only asserts "HwCD did not fire" passes for any reason
at all, including a broken `deliverEvent`. So the SAME event table, the SAME delivery entry point and
the SAME matching rule are used to show `0xF0000011` spec `0x0004` — the exact call `memcard.cpp:406`
makes — **does** fire. The card half and the CD half differ only in whether the class was opened.

**Observed to fail correctly**, per `docs/workspace/PROTOCOL.md`. A scratch copy with exactly one
planted row — `{0xF0000003, 0x00000040}`, the single thing the measured table lacks — turns the
negative case red, and reports which specs changed:

```
hwcd_cannot_fire_without_an_opened_slot
  FAIL inert == specCount: got 3 want 5
```

3 of 5, not 0 of 5: the three specs whose bits cover `0x40` (`0x40`, `0x0004`, `0xFFFFFFFF`) become
deliverable and `0x20`/`0x8000` do not. The test is sensitive to the finding, not merely green.

**Two fixture defects were found and fixed by that discriminator, both of which would have shipped:**

1. The negative case first watched ONE nominated handle, so planting an HwCD slot left it GREEN — the
   delivery marks the HwCD slot and never touches the HwCARD handle being watched. It now watches the
   **whole open table**, which is the question the finding actually raises.
2. The cases shared one `Game`, so the 16-slot table filled partway through and later `OpenEvent`
   calls returned the "table full" handle; `TestEvent` on a handle that was never opened reads as
   "did not fire". A discriminator that passes for the wrong reason is worse than none. Each case now
   gets a fresh `Game`, and case 3 asserts the absence is not a capacity artifact by opening a tenth
   event successfully.

## 7. Behavioural evidence, and what did NOT move

Baseline, reproduced on the unmodified framework at `67f1af1c`, one bounded headless run:

| measure | value |
|---|---|
| submitted prims | **0** (issue 0026's standing measure, > 2) |
| non-black share | **0 / 691200 = 0.00%**, `scratch/screenshots/present_3500.png`, 960x720 headless sink |
| capture path | `spider1/tools/probe_spider1_headless_run.py --shot-at 3500`, `PSXPORT_PRESENT_SINK=960x720`, offscreen, silent, unpaced |
| **second STR field** | **NO.** Still `resumed retail STR field 1 at 0x8002AC8C`, once |
| `pending I_STAT&I_MASK=0x004` | 1, then silent; no SysEnq element claims it (1 in chain) |
| `OpenEvent` classes | 3 (SwCARD/HwCARD/HwSPU), **0 HwCD** |
| `cdcr` reads of `0x1F801800-3` | **0** |

`cdcr` deserves its denominator: the sibling `cdc` channel logged **3** lines in the same run
(`setmode 0x80`, `setmode 0xE0`, `sector LBA 128304 -> data FIFO`), so the channel is live and the
guest genuinely never touched the controller — including never reaching the §3 poll at `0x8008C3E0`.

`PSXPORT_DEBUG=irq` was deliberately **not** used: the prior arm measured 1.28 GB in 90 s on this
stuck state, and the control surface is already known to wedge after its first command
(`dbg_server.cpp:855` clears `s_req_pending` but not `s_resp_ready`). Per-instant register reads were
therefore **not** taken; the CD numbers above come from the product's own `ev`/`cdc`/`irq` channels,
which report what the devices logged rather than the values sitting in them at one instant. That is a
weaker instrument than a register read and is named as such.

## 8. What this issue does NOT establish

- The three `DeliverEvent(0xF0000003, …)` calls in the image are **dead code on real hardware too**,
  for this title, for the same reason: nothing opens the class. They are standard Psy-Q libcd
  boilerplate, present but unused by a title that drives the callback-pointer path.
- Whether the 71-sector STR read is the actual blocker. The read is issued and the frame reaches
  field 1, so the frontier is downstream of it; the callback-pointer dispatch (§5) is the first
  unowned mechanism found, not a proven root cause.
- The `0x800B3B18` slot's runtime value during the stuck state. The image's initial value is `0`
  (`CdInit` writes it at run time), and the control surface could not be read on a stuck run.

## 9. Instrument defects hit while measuring (both pre-existing, in `psxport`)

- `dbg_server.cpp:855` — the abandonment path clears `s_req_pending` and leaves `s_resp_ready` set, so
  the endpoint serves one command per process for the rest of the run. Confirmed: `frame` was refused,
  and the driver's own report says "served 0 of 1 sample point(s) asked for".
- The `probe_spider1_headless_run.py` `--disc` argument takes the **CHD**, not the authenticated
  executable. Passing the `.exe` produces `failed to open CHD` and a misleading `CdRead: LBA 16
  unreadable` cascade. Worth a refusal in the tool.

## 10. Gate status

- `psxport`: **175/175 green** (was 174; +1 for the new test). `cpp_style` and `cpp_policy` pass;
  the new file was clang-formatted after the gate caught real violations.
- `runtime/psx/hle.cpp` is **unchanged at 748 lines**, still at its shrink-only cap. Nothing was
  added to it.
- `spider1`: 28 tests, 26 pass, **2 fail — `psxport_pin` and `spider1_psxport_pin_live`, both
  pre-existing.** Verified pre-existing by parking the new file and re-running with the framework
  tree **fully clean**: both still fail with `you built against 67f1af1c but this repo records
  43eac4bc`. Not bumped; that is the operator's.
- **Every port pin is already stale**, not just spider1's: `spyro`, `spider1`, `ctr`, `crashbash`,
  `megamanx4`, `tekken3`, `Tomba2Engine` and `vagrant` all record `43eac4bc` while the tree is
  `67f1af1c`; only `crash` records `67f1af1c`. Eight of nine, before any commit here.

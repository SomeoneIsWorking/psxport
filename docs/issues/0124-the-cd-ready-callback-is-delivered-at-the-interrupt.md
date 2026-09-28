---
id: 124
title: The CD-ROM ready callback is now delivered at the interrupt — the declared GuestInterrupt seam is implemented, it fires exactly once, and the 71-sector read was never the blocker
symptom: unchanged picture (0 submitted prims, 0/691200 non-black) and unchanged frontier (retail
         STR field 1 only), so the fix is a dead end for the BLACK FRAMES — but the delivery itself
         is measured working, and the next blocker is named.
state_items: S013,S017,S019
tags: cd,interrupt,ready-callback,libcd,frontier,dead-end,root-cause,s017
created: 2026-09-28
updated: 2026-09-28
---

## Answer

**The seam is implemented and it works; the black frames are NOT caused by it, and the 71-sector STR
read is NOT blocked by anything at all.** Measured on Spider-Man 1 (`SLUS_008.75`) with the framework
change built: the framework's stand-in for the BIOS's own CD-ROM interrupt handler fired **once**, on
the one completion the controller owed, and dispatched the **current** value of the registered slot —
`0x800860B4`, the function libstr installs during a stream, which is exactly what
`GuestCdStreamCallbackLayout::readyCallbackPointer` and the layout's comment predicted. The
`HostPump` control still receives exactly one callback per completion through the host pump and
through no other path.

**What did not move, and this is the finding:** the stream does not continue. The controller produced
**one** sector in the whole run and was never asked for a second, the guest took **zero** reads of
`0x1F801800-3` (`cdcr` = 0 lines with the channel demonstrably live), and **DMA3 completed zero
times**. The guest's own libcd interrupt service therefore still never runs; the framework's callback
is the only consumer, and whatever the guest's stream callback needs in order to continue is a
**title-side** question, not a framework delivery one.

## 1. What now delivers the callback, and where

`runtime/psx/cd_ready_delivery.{h,cpp}` (new, 2 files), called from `Hle::irqPoll` in
`runtime/psx/hle_interrupt.cpp`.

`Hle::irqPoll` already services DMA completions lowest-channel-first as its first act. The CD arm is
its sibling and follows the same two rules that arm's own comments state, because a CD chain breaks
for the same reasons a DMA chain did:

- **a completion is only consumed when it is delivered.** `consumeCurrentResponse` reads the
  controller's response FIFO and the controller acknowledge (the write that POPS the response queue)
  happen *inside* the delivery, after every refusal has been passed. A masked CD line, a re-entrant
  poll, an uninstalled callback, or a response that is not data-ready all return without touching the
  queue.
- **the guest's handler may re-enter the CD owner without the delivery re-entering itself.**
  `Hle::in_irq` is set for the whole extent of the dispatched callback, and the completion chain
  depends on it: a callback that issues the next request gets its response current *before it
  returns*, so a re-entrant poll must find the completion owed, not taken.

Delivery happens with the whole `R3000` context saved and restored around the guest function, exactly
as an exception entry would, and acknowledges the controller BEFORE dispatching — the order the
hardware's own handler uses, and the order that matters because the acknowledge is what can raise a
fresh edge for a response already queued behind this one. That edge is folded through
`Core::irqStatLatch()` and `PW_IRQ` re-armed, so a queued second completion is visible to the next
poll instead of waiting for an unrelated I_STAT access.

Arguments: `$a0 = 1` (the libcd completion code) and `$a1 = 0` (no first-word pointer). Both
existing host-pump delivery sites in the framework — `Cd::pumpStream` and `cd_drive_stock_read` —
already pass exactly this pair, and a title that saw two different argument shapes from one slot
depending on who called it would be a worse defect than one that sees a constant. Read from
SLUS_008.75's own bytes: the two functions its `CdInit` installs into the slot, and libstr's
replacement for it, read **neither** argument.

### Double-delivery is impossible, and here is the control

Three owners, and each completion is delivered by exactly one of them:

| owner | what delivers | evidence |
|---|---|---|
| `GuestInterrupt`, guest's own element claims the CD bit | the guest's element, once | `guest_element_keeps_its_own_single_delivery` and `a_claiming_guest_element_still_blocks_the_framework_arm`: `cd_ready_delivered == 0` |
| `GuestInterrupt`, no element claims it | this arm, once | `one_completion_is_delivered_exactly_once`: 3 polls, 1 callback |
| `HostPump` | `Cd::pumpStream`, once | `host_pump_owner_gets_exactly_one_callback_from_the_pump`: after `irqPoll` **0** callbacks and the response still owed, after `pumpStream` **1**, after a further pump and poll still **1**, and `cd_ready_delivered == 0` — the arm never ran at all |

The `HostPump` control is the one a test of only the new path cannot provide, and it is checked
through the **shipping poll with a callback count**, before the host pump ever runs, so a framework
that delivered everything twice fails with the number `1` — the second delivery — rather than with a
shape mismatch. A legacy `GameConfig` consumer declares nothing and is never owned by the interrupt
(`legacy_consumer_is_not_owned_by_the_interrupt`).

**In the product, the same exclusion holds by construction and was measured:** `Cd::pumpStream`
returns before reaching its own dispatch for a `GuestInterrupt` declaration, so the run's single
callback could only have come from the interrupt arm, and the `cdirq` line names the callback
address `0x800860B4` — the slot value, which only the interrupt arm reads at the interrupt.

`tests/test_cd_ready_delivery.cpp`: 13 cases, 126 checks, hermetic. The pre-existing
`tests/test_cd_stream_callback.cpp` (5 cases, 49 checks) is unchanged and still green.

### Observed to fail correctly

Five discriminators, each reverted after use:

| planted defect | result |
|---|---|
| the whole `DeliveryOwner` gate removed | 3 cases red; the HostPump control red at `callbackCalls == 0: got 1` — **a measured double delivery** |
| only the `core.cfg` half of the gate removed | **nothing red** — reported, not hidden. No live fixture has a `cfg` AND a direct runtime, so that half of the gate is defence-in-depth no test can currently reach |
| the controller acknowledge (which pops the response) moved BEFORE the refusal checks — "take the completion, then decline the dispatch" | 4 cases red on the owed-response assertions, including the chain case |
| the `in_irq` re-entrancy refusal removed | 2 cases red |
| the `!claimed` order gate removed from `Hle::irqPoll` | 1 case red, again at `callbackCalls == 0: got 1` |

Two of the first discriminators initially passed, and both times the reason was the fixture rather
than the gate: consuming the response FIFO does not pop the controller queue (only the interrupt-flag
acknowledge does), and a guest element that ACKNOWLEDGED I_STAT made the arm look correct for the wrong
reason. Both fixtures were corrected before the discriminator was believed.

## 2. Behavioural evidence on SLUS_008.75

Capture path: `spider1/tools/probe_spider1_headless_run.py --disc "<the CHD>" --frames 4300
--native-frames 4300 --shot-at 3500`, `PSXPORT_PRESENT_SINK=960x720`, `SDL_VIDEODRIVER=offscreen`,
silent, unpaced, one captured PID, product reaped by the driver.

| measure | before | after |
|---|---|---|
| submitted prims | 0 (`frames 3500..3500 passed with ZERO prims offered`) | **0** (`frames 3400..3600 passed with ZERO prims offered`) |
| non-black share | 0 / 691200 = 0.00% | **0 / 691200 = 0.00%**, re-derived from `scratch/screenshots/present_3500.png`'s 691200 pixels, not from the log line |
| **second STR field** | **NO** | **NO** — `resumed retail STR field 1 at 0x8002AC8C` still exactly once |
| 71-sector STR read from LBA 397 | issued | **issued and COMPLETED** — `[cd] CdRead 71 sector(s) x 2048 bytes from LBA 397 -> 0x800FCA9C (mode 0x180)`, served natively from the real disc image |
| CD ready-callback deliveries | 0 (the promise in `pumpStream` was unkept) | **1 of 1 owed completions**, to `0x800860B4`, controller status `0x22`, `a0=1 a1=0` |
| controller sectors queued | 1 (LBA 128304) | **1** (LBA 128304) — never asked for a second |
| `cdcr` reads of `0x1F801800-3` | 0 | **0**, with the sibling `cdc` channel live at 3 lines in the same run |
| DMA3 completions | 0 | **0**; DICR armed once, `0x00920000 -> 0x009A0000` (mask `0x1A`) from `ra=0x80086D90` |
| presented frames | 4,199+ | 4,300+ |

## 3. The CD channel state, with denominators

Over one 4,300-native-frame run with `PSXPORT_DEBUG=cdirq,cd,cdc,cdcr,dmairq` and **66 log lines
total**, so no channel is flooding and every count below is a count of lines, not of polls:

- `[cdirq]` **1** line: 1 delivery, 0 deferrals, 0 "nothing delivered" — 1 of 1 owed completions.
- `[cdc]` **3** lines: `setmode 0x80`, `setmode 0xE0`, `sector LBA 128304 -> data FIFO`. Every one of
  the three comes from the **native** layer's own `cdc_begin_read`/`cdc_set_mode`; no guest CD command
  ever reached the controller's command register.
- `[cd]` **9** lines: the pump installation, the `CdInit` mask arm, and the 7 `CdRead` calls of
  issue 0026 — LBA 16, 18, 22, 390 x7, 128303, 22, and **397 x71**.
- `[cdcr]` **0** lines, and the channel is live (the sibling `cdc` channel logged 3 in the same run),
  so this is "scanned, matched 0" and not "the instrument never ran". **The guest still never reads a
  CD controller register, so its own libcd interrupt service is still unreachable.**
- `[dmairq]` **15** lines, every one of them before the delivery; **no DMA3 completion after it**.

Two further measurements, through the control surface, in separate bounded runs:

- a 13-word read at `0x800C14E0` returned `[0x800C14F4] = 1` and `[0x800C1508] = 0` — the two words the
  guest's stream callback `0x80085000` tests first, in that order, and both in the state that lets it
  proceed past both of them. So the callback ran past its entry guards and still produced no
  controller traffic and no DMA3.
- a 6-word read at `0x800B3B10`, taken AFTER the delivery, returned
  `00450000 8008A238 800860B4 00000000 00000000 00000000` — so `[0x800B3B14]` still holds the
  `CdInit` sync callback `0x8008A238` and **`[0x800B3B18]` still holds `0x800860B4`**, the address the
  delivery dispatched. The callback was therefore not uninstalled, and the blocker is not a cleared
  slot: the guest's stream simply never asks the controller for another sector.

## 4. Comments corrected, quoted

**`Cd::pumpStream` — the unkept promise, now kept.** Was:

> The controller, not the host pump, raises INT1. The guest libcd ISR must consume its response
> before invoking the ready callback; irqPoll delivers it at a safe boundary after this native call
> returns.

Now:

> The controller, not the host pump, raises INT1, and the response must be consumed before the ready
> callback runs or CdReady observes a stale libcd result. That promise is now KEPT, by
> `Hle::irqPoll`: once the guest's registered interrupt elements have declined the CD bit,
> `cd_ready_delivery.cpp` — the framework's stand-in for the BIOS's own CD-ROM interrupt handler,
> which is ROM code this port does not have — acknowledges the controller and dispatches the CURRENT
> value of the registered slot. It is gated on this same declaration, so the two paths are exclusive
> rather than additive, and it defers on `in_irq`, so a chained sector cannot deliver twice.

**`Cd::hleInit` — was unconditional, now conditional and specific.** Was:

> The callbacks are dead in our model (no IRQ invokes them; every command completes inline), but we
> install them so any code that inspects the table sees the same values as on real hardware.

Now:

> WHETHER THOSE CALLBACKS ARE DEAD DEPENDS ON THE DECLARED DELIVERY OWNER, and this is the one place
> the answer differs between the two. For a legacy `GameConfig` consumer they are dead: every command
> completes inline, nothing raises a CD-ROM interrupt, and no IRQ invokes them. For a DIRECT runtime
> that declares `GuestCdStreamCallbackLayout::DeliveryOwner::GuestInterrupt` they are NOT dead —
> `cd_ready_delivery.cpp` is the framework's stand-in for the BIOS's own CD-ROM interrupt handler, and
> it dispatches the registered ready-callback slot at the interrupt.

The pre-existing `guest_cd_stream_callback_layout.h` header comment, which said the guest's own ISR
"consumes the response and then invokes the registered callback", is still true of the **effect**; the
new module's header says plainly which half of that is the framework's, and which response types it
does not serve.

## 5. A second framework defect, fixed here because it is what made the last two arms weak

`runtime/psx/dbg_server.cpp` — the control surface served **one command per process** for the rest of
a stuck run. `dbg_submit`'s abandonment path cleared `s_req_pending` and left `s_resp_ready` set; the
main thread's `service()` then completed the abandoned slot and set `s_resp_ready` for nobody, and the
next submitter's guard loop `while (s_req_pending || s_resp_ready)` spun to its own timeout forever.

Fixed with a request **generation** (`mReqGen` / `mRespGen`): the guard waits only for a result owed to
*this* generation, the abandonment path bumps the generation so an in-flight service's result is
discarded instead of published, and `service()` drops a result whose generation has moved on rather
than leaking it. This is a diagnostic-transport change and cannot affect the product's execution path
(the endpoint is off unless `PSXPORT_DEBUG_SERVER` names a port).

**Verification is end-to-end, not a unit test**, and that is the honest description: a hermetic test of
this handshake needs the endpoint's threads and a running product, which this repository's hermetic
rule excludes. The measured before/after, on the same stuck Spider-Man state, two reads asked on two
separate connections:

- before: **served 1 of 2**; the second read spun to its own timeout.
- after: **served 2 of 2**, both replies carrying words.

That is a behavioural control on the symptom, not a proof of the concurrency argument; the argument is
in the code comments. The scratch probes that took it are `spider1/scratch/headless/one_read_probe.py`
and `two_read_probe.py` — deliberately scratch, not tracked tools, because a probe that exists only to
prove a fix to its own fix is a liability rather than a gate.

## 6. Instrument defects confirmed, and one tool refusal still wanted

- `PSXPORT_DEBUG=irq` was **not** used (1.28 GB in 90 s, measured by an earlier arm). Every number
  above comes from `cdirq`/`cd`/`cdc`/`cdcr`/`dmairq` over a 66-line log.
- Per-instant register reads: the ONE read in §3 was taken through the control surface, and the
  `probe_spider1_headless_run.py` driver's own sampler served **0 of 0** sample points on every run,
  which its report states. The one read came from a scratch script asking for a single 13-word block
  rather than the driver's nine single-word reads, which is the difference between a measurement and a
  timeout.
- `probe_spider1_headless_run.py --disc` takes the **CHD**. Passing the `.exe` reproduces the
  misleading `CdRead: LBA 16 unreadable` cascade. A refusal in the tool is still outstanding and is
  title-side work.

## 7. What this issue does NOT establish

- **That the ready-callback delivery is the cause of the black frames.** It is not, on this
  measurement: the delivery happens, the picture does not change, and the frontier does not move. The
  callback-pointer route was the first *unowned* mechanism found, not a proven root cause, exactly as
  issue 0123 said it would be.
- **Why the guest's stream callback does not continue.** Its two entry guards are passed and it
  produces no controller traffic, no DMA3 and no further sector. That is inside the guest's STR
  playback path and needs title-side RE; it is not answerable from the framework's delivery.
- **Whether the controller's data-FIFO request latch is the next blocker.** The guest's CD register
  writes in this run are attributed by the `cdcw` channel to a PC that is really a `jal` return
  address, so their caller is unknown; and the vendored Beetle model — the one this controller model
  follows — computes `reg_index = (RegSelector & 3) * 3 + (A - 1)`, so index 2 and 3 land in
  `default: break` there too. **The framework is not diverging from its own vendored reference on
  this, and "the banking is wrong" would be a wrong conclusion.** Whether the guest *needs* those
  registers to be live is unknown.
- **The CD channel's per-instant register values at the moment of the delivery.** The control surface
  was used once, after the delivery, and the driver's own sampler could not be made to serve a sample.

## 7a. Gate status, stated exactly

`psxport` `ctest --test-dir build`: **179/179 GREEN**, run twice in a row at the end. It was 175
before this change; the count rose by four because two other arms' new suites registered in the same
window.

**A note on reading that number, because the tree is not a fixed object.** Other arms were editing
`runtime/psx/`, `tests/`, `cmake/psxport.cmake` and `tools/check_cpp_style.py` throughout, and several
intermediate readings were red for reasons that had nothing to do with this change:
`tests/test_ordering_table.cpp` did not compile (`std::vector` with no `#include <vector>`),
`test_gp0_command` and `test_ordering_table` were briefly "Not Run" while a rebuild was in flight, and
`cpp_style` was red on seven other arms' files. Every one of those cleared on its own. The figure
above is the settled state, and the only reading of it that is honest is: this change's own suite is
13 cases / 126 checks and green in every single run, including the ones taken while the rest of the
tree was red.

The eight files this change touches are each format-clean under the repository's own
`clang-format --dry-run --Werror` and the repository's `.clang-format`, and `clang-tidy` with the
tracked `.clang-tidy` over the real compile database exits 0 on all four touched translation units.
`tools/check_cpp_style.py`'s cap table was edited by another arm in this window (`gpu_native.cpp`
4030 -> 3592, `render_queue.cpp` 2178 -> 1819 — two shrink-only ratchets); `"runtime/psx/hle.cpp":
748` is untouched and `hle.cpp` is still 748 lines.
`runtime/psx/hle.cpp` is **unchanged at 748 lines**, still exactly at its shrink-only cap; the new
responsibility went into its own module rather than into a capped file. One correction to the framing
this work was given: `runtime/psx/hle_interrupt.cpp` is **not** under a shrink-only cap — it is not in
`PSXPORT_CAPS` at all — so it went from 252 to 287 lines against the 1,200-line default, with no cap
raised and no ratchet owed. `runtime/psx/dbg_server.cpp` went from 1,033 to 1,067, also under the
default.

`spider1` `ctest --test-dir build/agent-clang`: **26/28**, the two reds being `psxport_pin` and
`spider1_psxport_pin_live`, both with the same message:

> framework ... is **dirty** or changed since configure (configured `80e917b3`, current `80e917b3`)

Same commit on both sides, so the *only* cause is the uncommitted working tree — this change, plus the
other arms' untracked files. `tools/pin_round.py --dry-run` reports all nine ports agreeing with the
framework at `80e917b3`; nothing is stale **yet**, and all nine go stale at the moment this is
committed, which is the guard working. `toystory2` records an older commit (`82ab61f8`) and is outside
the round's list. No pin was bumped and no pin was hand-edited.

## 8. What the consuming repository's registries should say

For `spider1`:

- `docs/issues/0026` §6's claim that `Hle::deliverEvent` must gain an HwCD arm is **refuted** and
  already is by psxport issue 0123; this issue closes the rest of it. §6's proposal (2) — "`Hle::irqPoll`
  must invoke the BIOS CD-ROM ready-callback slot on IRQ2" — is now **implemented in the framework**,
  through `readyCallbackPointer` and NOT through a new event class, and it fires.
- The standing measure in `docs/issues/0025` and §7 of 0026 is **unchanged**: 0 submitted prims,
  0.00% non-black, retail STR field 1 only. A new issue is warranted for the measured next blocker:
  the stream callback runs past its entry guards and then the controller, the DMA channel and the
  guest's own CD service all stay idle, with the `cdcr`-zero and DMA3-zero denominators above.
- `spider1/tests/cd_irq2_delivery_test.cpp`'s `!hwcDAmongRaised` baseline **stays true** and its test
  is not to be inverted: this change adds no BIOS event class, which is the whole point.

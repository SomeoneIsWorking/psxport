---
id: 147
title: A stock-libcd title stalled forever on three CD owners that each believed they were the only one
symptom: C-12's startup read pump retried the same sector every 60 fields, then reported
  `CdRead: sector error`, then stalled with no completion at all
state_items: S003,S004
tags: cd,interrupt,cdc,request-latch,command-order,code-module,root-cause,c12
created: 2026-10-01
updated: 2026-10-01
---

## Answer

**Four separate ownership defects, all in this framework, kept one stock-libcd title from ever
seeing a sector.** Each was found by observing what the controller did, not by reading the guest.

1. **No completion owner at all.** `deliverCdReadyCompletionOnInterrupt` serves only a title that
   declares `GuestCdStreamCallbackLayout`. C-12 declared none, so its `CdReadyCallback` slot was
   never read. (Title-side declaration; recorded in the consumer, not here.)

2. **Two owners for one sector.** `cd_drive_stock_read` bursts the ready callback out of
   `CdControl(ReadN)` WITHOUT touching the controller, so the controller's data-ready response stays
   owed; `Hle::irqPoll` then delivered that stale response again to whatever callback the guest had
   since installed — measured delivering to `0x800A8E90`, the value the guest had just restored,
   after the read had finished. `Cd::pumpStream` already made host-pump and guest-interrupt delivery
   exclusive; the finite-read burst did not, and neither did the reverse case. Both paths now ask the
   one owner question (`cdReadyCallbackOwnedByGuestInterrupt`).

3. **A command the framework had already answered ran AFTER the next one.** The synchronous command
   owner answers the guest inside the call, but `cdc_issue_command` scheduled the controller's Pause
   on the guest clock. C-12's read pump issues Pause, Setloc, Setmode, ReadN; the Pause executed
   after the ReadN had already begun the next read and cancelled that read's own sector event, so
   no completion ever arrived. The controller takes its own time to execute a command and
   `test_cdc_continuous_read` pins that a native-issued Pause observes the still-reading status at
   execution and the paused status at completion, so the command stays TIMED. The ordering was the
   actual defect: a controller executes the commands it accepted in the order it accepted them, so
   `cdc_begin_read` now retires anything still pending through the same execute/completion phases
   before the read it was issued after begins (`retire_pending_command`). Without that, C-12 streams
   two sectors and stops; with it, it streams the whole 121-sector read.

   An earlier attempt fixed this by completing the command in line at issue, which broke the pinned
   timed contract and had to be undone. A second, wrong attempt then deleted the command's own
   responses from the FIFO on the premise that a framework-issued command has "no guest consumer" —
   it is the GUEST that issued it through its own CdControl, and stock libcd polls its status after
   every command (measured: Setloc -> Setmode -> ReadN -> Pause, each followed by a GetStat poll).
   Deleting those responses left the guest waiting, after its own Pause, for a completion that had
   already been thrown away; that version stalled after two sectors as well. Both were measured and
   removed: responses belong to the guest that asked for them, and a data-ready behind an unread
   command response stays behind it, exactly as hardware serves the FIFO in order.

4. **A repeated request-register write did nothing, then everything.** The request bit was modelled
   as a pure latch (psxport issue 0002, Crash Bash's split DMA). But stock libcd writes it once per
   sector and never clears it in between, so every sector after the first was never presented: the
   guest read sector N's EDC/ECC tail back for sector N+1 (`CdRead: sector error`), and once the
   payload boundary was respected, the drive never re-armed and a 121-sector read stopped at three.
   A request now presents the announced sector once the guest holds the previous sector's payload,
   and that handoff re-arms the drive's own clock. A split DMA that has not reached the payload
   still reads the same sector, which is what issue 0002 measured and still measures.

Bonus, same class: **the title loads code into RAM.** `resolveHostDispatch` admits a guest call only
when an active image owns the address, so a title whose own loader reads a relocatable module from
the disc and calls its entry was refused with `ambiguous code-image identity` — while the identical
call worked for resident text. `guest_code_module.*` establishes the residency when a CD transfer
lands inside the window the title declares.

## Evidence

On the authenticated C-12 image, before: 700 turns, 0 frame boundaries, `CdRead: retry...` every
60 fields. After: 2,000 turns, 1,038 frame boundaries, 92.5 M guest instructions, 0 fallback
blocks, 0 faults, no retry and no sector error, the disc-loaded `RELOCS/GT.LVB` module executing
from `0x8011F9BC`, and the guest submitting drawing primitives.

## Notes

Nothing retires the controller's response FIFO. The delivery path only consumes the completion it is
delivering, and it does that only when the CURRENT response is a data-ready, so an unread command
response keeps its place in front of it — the guest's own status polls consume it and the
completion becomes current. This is the second measured false premise of this issue, recorded here
because the symptom (a title whose reads complete once and then never again) is the same either
way.
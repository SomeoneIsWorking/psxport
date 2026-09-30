---
id: 143
title: A synchronous stock CdRead never raises the completion a chained loader waits for, so Spyro 2 and 3 spin forever in their module loader
status: fixed
symptom: Spyro 2 (SCUS_944.25) and Spyro 3 (SCUS_944.67) stop in the loader's poll after one CdRead; cd_ready_delivered + cd_ready_declined = 0 and zero cdirq lines for the whole run, against a real disc fill
state_items: S006,S024
tags: cd,stock-read,ready-callback,interrupt,guest-interrupt,root-cause,spyro
created: 2026-10-01
updated: 2026-10-01
---

## Root cause

`cd_read_stock_sync` performs the whole CdRead from the disc image and never touches the CD controller: no
response is queued, no INT1 rises, and the stand-in for the BIOS CD-ROM interrupt handler
(`cd_ready_delivery.*`, issue 0124) has nothing to deliver. A guest that only polls a flag does not care.
Spyro 2 and 3 issue ONE CdRead, spin on a bytes-so-far word, and start every further read from the ready
callback they registered with `CdReadyCallback` (Spyro 2: slot `0x800663B8` -> `0x8001379C`; Spyro 3:
`0x80050504`). On retail the BIOS handler runs that callback once per completed read.

Two further facts, both measured on Spyro 2, were needed before the completion could be delivered:

1. **The callback's argument is not 1.** `0x8001379C` does `andi $v1,$a0,0xff` and compares with 2: 2 ends
   the read (it clears the in-progress word `0x800682E8`), anything else starts the NEXT read. The arm
   delivered a constant `$a0 = 1` (libcd's data-ready code, right for Spider-Man 1, whose callback ignores
   it), which would chain forever. The code is now declared by the title.
2. **The CD line was never open.** The first stock read ran with I_MASK `0x009` (VBlank and DMA). Retail
   `CdInit` reaches the BIOS interrupt-enable; Spyro 2 replaces `CdInit` with a native success body that did
   not. `cdReadyCompletionOwed` keeps the hardware's own mask gate, so the queued completion sat owed, unowned,
   for the whole run. Spider-Man 1 had the same gap and closed it title-side (`armCdInterrupt` in
   `spider1_cd_initialization.cpp`); the rule now has one framework owner.

## The change

| piece | where |
|---|---|
| two declared fields on `GuestCdStreamCallbackLayout`: `readyStatus` (default 1) and `stockReadRaisesCompletion` (default false) | `runtime/psx/guest_cd_stream_callback_layout.h` |
| `psx::cd::raiseStockReadCompletion`, the one owner: after a successful stock read of >0 sectors, post one INT1 data-ready response, latch the edge, request an IRQ poll | `runtime/psx/cd_stock_read_completion.*`, called at the end of `cd_read_stock_sync` |
| `cdc_post_data_ready`: queue ONE data-ready response without starting a drive read; refuses (returns 0) when the ring is full | `runtime/psx/cdc_native.cpp` |
| the delivery arm passes the declared `readyStatus` instead of a constant | `runtime/psx/cd_ready_delivery.cpp` |
| `psx::cd::armCdInterrupt`: the one implementation of "open I_MASK bit 2 through the device, keep the guest's other bits" | same module |
| `cd_read_stock_sync` reads sectors through `cdc.disc_read_raw_fn` (defaults to `disc_read_raw`) so the shipping function is testable with fake sectors | `runtime/psx/cd_override.cpp` |

**Exactly once per read is enforced by the controller queue, not by a counter.** One successful read posts
one response; the delivery consumes and acknowledges that response before dispatching; `in_irq` defers a
response posted from inside the callback until the callback has returned (the chain), and it stays owed. A
read of zero sectors, a failed read and a full queue post nothing, and the full-queue case logs an error
rather than counting a completion that does not exist.

**Who is unaffected, byte for byte:** a legacy `GameConfig` consumer, a `HostPump` title, and a
`GuestInterrupt` title that does not opt in. Spider-Man 1 declares `GuestInterrupt` but its libstr callback
must not run once per stock read, so it is deliberately NOT opted in by the new flag; this is why the seam
is a separate declaration and not implied by the owner.

## Evidence

`tests/test_cd_ready_delivery.cpp` (22 cases, the original 13 plus 9): N stock reads give N deliveries with
the declared `$a0`; a 7-sector read owes one; a chained loader (callback starts the next read) completes
exactly its reads and stops; and the negatives, each required to come out zero: declared owner without the
flag, `HostPump` with the flag, legacy consumer, zero-sector and failed reads, a full queue, and a masked
line (completion stays owed until `armCdInterrupt`, then delivers once, the guest's other mask bits intact).
Removing the call in `cd_read_stock_sync` turns exactly the three positive stock-read cases red (3 of 21 at the time of the discriminator).

Spyro 2 on a real disc, headless: the two reads the guest issues (1 sector at LBA 500, 37 sectors at LBA 570)
give `cd_ready_delivered = 2`, `declined = 0`, `$a0 = 2`; `[0x800682E8] = 0` afterwards; the module load
`0x80013810` returns. The boot now stops at `0x80077374` (`ambiguous code-image identity`), which is the
next frontier and is title-side: the loaded module's identity.

## Not established

- Spyro 3 was not run against this change by the framework commit itself; its title must declare the same two
  fields (`readyStatus = 2`) and arm the line from its native CdInit, if it replaces CdInit.
- A multi-sector stock read owes ONE completion; retail's per-sector cadence for multi-sector reads is not
  reproduced, and no measured title needs it.
- Spider-Man 1's own `armCdInterrupt` copy should migrate to `psx::cd::armCdInterrupt`.

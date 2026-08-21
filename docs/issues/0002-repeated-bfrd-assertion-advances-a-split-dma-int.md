---
id: 2
title: Repeated BFRD assertion advances a split DMA into the next sector
status: resolved
symptom: Crash Bash reads the LBA16 PVD header then receives LBA17 bytes for the payload and loops Cant find CRASHBSH.DAT
tags: cdc,cdrom,dma,request-latch
created: 2026-08-21
updated: 2026-08-21
---

## Root cause

`cdc_write` treated every write containing BFRD=1 as a new sector request. BFRD is a request latch:
an asserted-to-asserted write must leave the active data FIFO and its cursor alone. Crash Bash splits
one whole-sector transfer around such a repeated write, so the model discarded LBA 16 after only 12
bytes and reloaded LBA 17 before the 2048-byte payload DMA.

## What was tried / dead ends

The CHD and ISO9660 payload are not corrupt: keeping the cursor at raw LBA 16 offset 24 produces PVD
type 1 followed by `CD001`. A game-local `CdSearchFile` replacement would only hide the shared
request-register defect and was not added.

## Resolution

### Note (2026-08-21)
Measured consumer sequence: BFRD 00→80 at whole-sector LBA16, DMA3 reads 3 words (`data_rd=12`),
repeated 80 while asserted, then DMA3 reads 512 words. Before the fix the hermetic shipping-path test
advanced `loc_lba` to 17 on that repeated write; after modeling BFRD as a latch it stays at LBA16 and
returns raw offsets 12..2071. The test also deasserts then reasserts BFRD and proves that real
transition advances to LBA17, ruling out a blanket ignore.

### Resolution (2026-08-21)
BFRD was modeled as an action on every asserted write instead of a request latch. `cdc_write` now
preserves the current FIFO cursor on repeated `0x80` and advances/reloads only on deassert-to-assert.
The hermetic shipping-path split-DMA suite passes 3/3 and 535 checks, including the opposite
transition outcome and deasserted-FIFO access refusal. Real Crash Bash validation reaches the 512-word PVD DMA at LBA16 with
`data_rd=12` and bytes `01 CD001 01 00`; the 105-line consumer log has 0
`Cant find CRASHBSH.DAT` messages and 1 `load file start`, then boot reaches its next guest wait.
Consumer evidence: Crash Bash `docs/issues/0005` and
`scratch/logs/cdc-latch-{consumer,gdb}.log`.

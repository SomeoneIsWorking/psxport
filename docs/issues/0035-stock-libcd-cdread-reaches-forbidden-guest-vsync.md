---
id: 35
title: Stock libcd CdRead reaches forbidden guest VSync queries
status: resolved
symptom: CTR passes BIOS memcmp, then aborts at fatal VSync(-1) from RA 0x8007705C inside stock CdRead 0x80076F10; static follow-through finds two more queries in CdReadSync 0x800770AC.
tags: cd,libcd,vsync,frame-loop,ctr,hle
created: 2026-08-27
updated: 2026-08-27
---

## Root cause

The shared synchronous stock-libcd implementations already replace the whole drive wait and query state machines, but both functions were private to cd_override.cpp and only legacy GameConfig registration could bind them. A direct runtime therefore had no narrow DRY target for its measured CdRead and CdReadSync entries and fell through to the generated bodies, whose VSync(-1) status polls correctly hit the fatal native-frame-loop trap.

## Resolution in shared tree

cd_control.h now exposes cd_read_stock_sync(Core*) and cd_readsync_stock_sync(Core*), and legacy registration binds those same functions. The CdRead owner reads through disc_read_raw, which already lazily opens the Game DiscState using its env_key, so direct runtimes need no title-local open wrapper. test_cd_stock_read drives the shipping functions and covers missing-Setloc refusal without writes, successful zero-sector completion state, and CdReadSync remaining=0 plus result clearing.

## Evidence and live gate

2026-08-27: focused Clang build linked current libpsxport and test_cd_stock_read passed 1/1 in 0.07 seconds. CTR must bind 0x80076F10 and 0x800770AC and rerun; keep this issue investigating until the live run passes both forbidden query sites.

### Resolution (2026-08-27)
CTR real-disc PID 2665477 opened the disc and passed both former guest-VSync sites through shared cd_read_stock_sync at 0x80076F10 and cd_readsync_stock_sync at 0x800770AC. The process later exited 139 at an independent VSync(-1), PC 0x80075350 from RA 0x800750B8, before any present. This live falsifier closes the stock-libcd ownership issue without weakening the fatal VSync contract.

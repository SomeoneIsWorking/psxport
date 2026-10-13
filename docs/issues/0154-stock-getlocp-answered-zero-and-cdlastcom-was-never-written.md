# 0154: stock GetlocP answered zero and CdLastCom was never written

Status: fixed

## Observed
Spyro 1, same run as 0153. After the level flyby is cancelled the music driver pauses and resumes its
track with `ReadS` at `00:02:00` (LBA 0) instead of where it stopped (LBA ~60000). No audio sector is ever
decoded again: the level has no music. `cdcmd` shows `CdControlF(GetlocP)` every frame.

## Cause
Spyro 1's `CDMusicUpdate` (open-spyro `music.c`) updates its track position only when `CdLastCom() ==
CdlGetlocP` and `CdSync`'s result holds a position, and resumes from that position. Two framework defects
broke it: `runtime/psx/cd/stock_cd_response.cpp:stock_cd_begin_command` published an all-zero response for
GetlocP, and `runtime/psx/cd/stock_cd_work_area.cpp:publishStockCommandWorkArea` never wrote libcd's
last-command byte, so `CdLastCom()` never returned GetlocP. The position stayed 0.

## Resolution
- `cdc_getloc_p` (cdc_native.cpp) is the one producer of the GetlocP bytes for the head's sector; the
  controller command and the stock response both use it.
- `StockCommandWorkArea::lastCommandAddress` is written with every command sent. Spyro 1 declares its
  libcd work area (`0x80074E50`/`0x80074E54`/`0x80074E55`, spyro issue 0182).

Tests: `test_cd_stock_toc` `getlocp_reports_the_head_position_through_sync` (failed before: 4 checks),
`test_cd_stock_workarea` `every_command_publishes_the_last_command_byte` (failed before).

After: the resume reads LBA 60000 and then 60099, and the level music plays continuously.

Other stock-libcd titles (Toy Story 2, Spider-Man, C-12) declare positions without `lastCommandAddress`;
add it where a title reads `CdLastCom()`.

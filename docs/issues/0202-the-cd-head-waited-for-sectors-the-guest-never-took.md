# 0202: the CD head waited for sectors the guest never took

Status: fixed

## Observed
Mega Man X4, the user's pad recording to player select (bug report `20261112-153928-music-not-playing`),
`PSXPORT_DEBUG=xa,cdcpace`: the player-select BGM (file 1 channel 0, LBA 226570, mode 0xC8) decoded one audio
sector and went silent; no sector event was armed after LBA 226572. The stream interleaves three audio channels
with five Form 1 filler sectors (submode 0x00) that the guest never reads.

## Cause
`runtime/psx/cd/cdc_native.cpp:CdController::serviceDueSector` stopped the head on an announced data sector until
the guest's BFRD request took it. Beetle's drive never waits (`PS_CDC_HandlePlayRead` reads over the sector
buffer and raises INT1 again), so a filler sector the guest acknowledges and ignores stalled the stream forever.

Holding the head had also hidden a second defect: an interrupt raised at a deadline-ended Lightrec segment waited
for the next block boundary, which a loop inside one block (X4's MDEC VLC decoder `0x800ED574`) does not reach
until the following deadline, so the guest's handler ran a sector period late and missed the next sector.

## Resolution
The head runs on its own clock: each serviced sector arms the next one a period after its own deadline, and a
sector still announced when the next is due is read over (`cdcpace` logs it with the interrupt then current).
`LightrecExecutor::executeWithBoundary` serves pending work at a deadline-ended segment before the guest resumes
(`servicePendingWorkAt`). Mega Man X4 runs its BIOS-thread tasks under the ChangeTh native, so its task fibers now
keep their own native nesting (psx-mega-man-x4 `bios_threads::Service::resumeFiber`) and interrupts reach task
code. After: the player-select BGM plays (-18 to -20 dB where it was silent), the title stream's data sectors are
all taken, and only the filler sectors are read over, each with its INT1 already acknowledged.

A host-paced stream keeps the hold. Tomba! 1 binds a base `StockLibcd` with a ready slot, so
`StockLibcd::pumpStream` stands in for libcd's INT1 service once per display field. A free-running head
read every `LOGO.STR` sector over before the pump ran, and the pump, which paces by the head's position,
saw nothing owed, so the movie task spun for 33 fields and the frame driver aborted. `StockLibcd::bind`
sets `CdController::head_holds_untaken` when the host delivers the ready callback (`readySlot != 0` and not
`deliversOnInterrupt`). The head then waits on an announced sector and a BFRD take re-arms it. After:
`LOGO.STR` runs guest fields 560 to 923 unpaced, LBAs 57830-58739 each serviced once with none read over
(psx-tomba `scratch/x4-fw-check2/t1b`).

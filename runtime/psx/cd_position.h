#pragma once

#include <cstdint>

// Where a PSX controller command's position parameter comes from, as pure arithmetic.
//
// THE RULE IS LIBCD'S, NOT THE CONTROLLER'S. `CdControl(com, param, result)` inspects a per-command
// table and, for the commands whose parameter is a disc POSITION, sends a Setloc(0x02) carrying that
// parameter BEFORE sending the command itself. So a game never has to issue Setloc explicitly, and a
// trace of the commands a game's own code issues will not contain one.
//
// MEASURED in Spyro 1 (SCUS_942.28), whose libcd CdControl is at 0x80063EAC and whose table is at
// 0x80074DAC (psx issue 0115, 2026-09-19). Its sound driver does
// `CdIntToPos(lba, &loc); CdControl(CdlReadS, &loc, 0)` at 0x800568D0-0x800568F0 and issues no
// Setloc at all. An override that replaces CdControl and reads only the command byte therefore
// throws the requested position away: the XA cursor stayed at LBA 0 while the guest had asked for
// LBA 113,448, and the stream scanned 53,874 sectors (~122 MB of CHD decompression, 3.0 s, no frame
// presented) before latching onto unrelated audio.
// CONSUMER NOTE: every port whose runtime binds cd_control_sync / the CdControl override inherits
// this. Tomba! 1's cd_native_startup.cpp does. It is libcd's own contract, so honouring it is the
// faithful behaviour, but only Spyro 1 has been re-verified against it — re-check a consumer's CD
// startup when its psxport.pin crosses 892e9550.
namespace psx::cd {

// The CD's first data sector is at 00:02:00, so an MSF carries a 150-sector lead-in offset.
inline constexpr int kLeadInSectors = 150;

// Whether libcd's CdControl sends an implicit Setloc for this command. Read from Spyro 1's table at
// 0x80074DAC; it is libcd's, so every PsyQ title shares it: SetlocL/SetlocP-style seeks and the
// reads that may carry a start position.
bool commandCarriesPosition(std::uint8_t command);

// BCD minute/second/frame -> LBA, or -1 when the MSF is before the first data sector.
int msfToLba(std::uint8_t mm, std::uint8_t ss, std::uint8_t ff);

} // namespace psx::cd

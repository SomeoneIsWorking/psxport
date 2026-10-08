// disc_file.h — the ONE ISO9660 file reader: a disc file's bytes, in sector order.
//
// `disc_find_file` answers WHERE a file is and `disc_read_sector` answers what a sector holds; the
// walk that turns those two into a file's bytes belongs to one place, and both of its callers live
// here — the CD extract a launcher provisions an executable with (`disc_extract_file`) and the BIOS
// LoadExec service (bios_load_exec.h), which loads the executable a guest named.
#pragma once

#include "disc.h"

#include <cstdint>
#include <vector>

namespace psx::cd {

// The largest file this reader will pull into memory: a PS-X EXE is at most its own 0x800-byte
// header plus main RAM, and this is also what a launcher's provisioning extract asks for. A
// directory record claiming more is refused rather than turned into a multi-gigabyte allocation.
inline constexpr std::uint32_t kDiscFileMaxBytes = 0x200000u;

// Read one file's whole content. Returns false and leaves `out` empty when the path is not on the
// disc, a sector read fails, or the directory record claims a size past `kDiscFileMaxBytes`.
// `isoPath` is an absolute ISO9660 path ("\S0\CRASH.EXE;1"); case, ";version", leading separators and
// "/" separators are all accepted, exactly as `disc_find_file` accepts them.
bool readDiscFile(DiscState &disc, const char *isoPath, std::vector<std::uint8_t> &out);

} // namespace psx::cd
// disc_file.h — the ONE ISO9660 file reader: a disc file's bytes, in sector order.
//
// `Disc::findFile` answers WHERE a file is and `Disc::readSector` answers what a sector holds; the
// walk that turns those two into a file's bytes belongs here.
#pragma once

#include "disc.h"

#include <cstdint>
#include <vector>

namespace psx::cd {

// The largest file this reader will pull into memory: a PS-X EXE is at most its own 0x800-byte
// header plus main RAM, and this is also what the executable provisioner asks for. A
// directory record claiming more is refused rather than turned into a multi-gigabyte allocation.
inline constexpr std::uint32_t kDiscFileMaxBytes = 0x200000u;

// Read one file's whole content. Returns false and leaves `out` empty when the path is not on the
// disc, a sector read fails, or the directory record claims a size past `kDiscFileMaxBytes`.
// `isoPath` is an absolute ISO9660 path ("\S0\CRASH.EXE;1"); case, ";version", leading separators and
// "/" separators are all accepted, exactly as `Disc::findFile` accepts them.
bool readDiscFile(Disc &disc, const char *isoPath, std::vector<std::uint8_t> &out);
// `length` bytes from `offset` into one file, for a part of an archive larger than `kDiscFileMaxBytes`. False, `out`
// empty, when the range is not inside the file, is longer than `kDiscFileMaxBytes` or a sector read fails.
bool readDiscFileRange(
    Disc &disc, const char *isoPath, std::uint32_t offset, std::uint32_t length, std::vector<std::uint8_t> &out);

} // namespace psx::cd
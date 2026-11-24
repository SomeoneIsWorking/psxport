// disc_provision.cpp — the disc backend's host-file side: `readDiscFile`, the one ISO-file-to-bytes reader
// (disc_file.h), and the *.chd drop-in scan through Fs.
#include "cd_position.h"
#include "disc.h"
#include "disc_file.h"
#include "fs_util.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace psx::cd {

namespace {

bool readSpan(Disc &disc, const DiscFile &file, uint32_t offset, uint32_t length, std::vector<std::uint8_t> &out) {
  out.resize(length);
  uint8_t sector[kDataSectorBytes];
  for (uint32_t done = 0; done < length;) {
    const uint32_t at = offset + done;
    if (!disc.readSector(file.lba + (at / kDataSectorBytes), sector)) {
      out.clear();
      return false;
    }
    const uint32_t within = at % kDataSectorBytes;
    const uint32_t chunk = std::min<uint32_t>(length - done, kDataSectorBytes - within);
    memcpy(out.data() + done, sector + within, chunk);
    done += chunk;
  }
  return true;
}

} // namespace

bool readDiscFile(Disc &disc, const char *isoPath, std::vector<std::uint8_t> &out) {
  out.clear();
  const std::optional<DiscFile> file = isoPath == nullptr ? std::nullopt : disc.findFile(isoPath);
  if (!file || file->size == 0u || file->size > kDiscFileMaxBytes) {
    return false;
  }
  return readSpan(disc, *file, 0, file->size, out);
}

bool readDiscFileRange(
    Disc &disc, const char *isoPath, std::uint32_t offset, std::uint32_t length, std::vector<std::uint8_t> &out) {
  out.clear();
  const std::optional<DiscFile> file = isoPath == nullptr ? std::nullopt : disc.findFile(isoPath);
  if (!file || length == 0u || length > kDiscFileMaxBytes || offset > file->size || length > file->size - offset) {
    return false;
  }
  return readSpan(disc, *file, offset, length, out);
}

bool findDropinImage(char *out, unsigned out_cap) {
  std::string found = Fs::findFirstWithExtension(".", ".chd");
  if (found.empty() || found.size() + 1 > out_cap) {
    return false;
  }
  memcpy(out, found.c_str(), found.size() + 1);
  return true;
}

} // namespace psx::cd

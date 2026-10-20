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

bool readDiscFile(Disc &disc, const char *isoPath, std::vector<std::uint8_t> &out) {
  out.clear();
  const std::optional<DiscFile> file = isoPath == nullptr ? std::nullopt : disc.findFile(isoPath);
  if (!file || file->size == 0u || file->size > kDiscFileMaxBytes) {
    return false;
  }
  const uint32_t size = file->size;
  uint32_t lba = file->lba;
  out.resize(size);
  uint8_t sector[kDataSectorBytes];
  for (uint32_t done = 0; done < size; done += kDataSectorBytes, ++lba) {
    if (!disc.readSector(lba, sector)) {
      out.clear();
      return false;
    }
    const uint32_t chunk = std::min<uint32_t>(size - done, kDataSectorBytes);
    memcpy(out.data() + done, sector, chunk);
  }
  return true;
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

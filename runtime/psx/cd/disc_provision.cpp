// disc_provision.cpp — the disc backend's host-file side: `readDiscFile`, the one ISO-file-to-bytes reader
// (disc_file.h), and the *.chd drop-in scan through Fs.
#include "disc.h"
#include "disc_file.h"
#include "fs_util.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace psx::cd {

bool readDiscFile(DiscState &disc, const char *isoPath, std::vector<std::uint8_t> &out) {
  out.clear();
  uint32_t lba = 0, size = 0;
  if (isoPath == nullptr || !disc_find_file(&disc, isoPath, &lba, &size) || size == 0u || size > kDiscFileMaxBytes) {
    return false;
  }
  out.resize(size);
  uint8_t sector[2048];
  for (uint32_t done = 0; done < size; done += 2048u, ++lba) {
    if (!disc_read_sector(&disc, lba, sector)) {
      out.clear();
      return false;
    }
    const uint32_t chunk = std::min<uint32_t>(size - done, 2048u);
    memcpy(out.data() + done, sector, chunk);
  }
  return true;
}

} // namespace psx::cd

extern "C" int disc_dropin_scan(char *out, unsigned out_cap) {
  std::string found = Fs::findFirstWithExtension(".", ".chd");
  if (found.empty()) {
    return 0;
  }
  if (found.size() + 1 > out_cap) {
    return 0;
  }
  memcpy(out, found.c_str(), found.size() + 1);
  return 1;
}

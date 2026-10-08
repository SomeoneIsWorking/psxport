// disc_provision.cpp — C-callable self-provisioning entry points for the disc backend (disc.c),
// implemented in C++ against Fs (game/core/fs_util.h) so host filesystem I/O goes through
// std::filesystem/fstream instead of hand-rolled FILE*/mkdir/dirent (USER directive 2026-07-14).
// It also hosts `readDiscFile`, the CD backend's one ISO-file-to-bytes reader (disc_file.h), because
// that walk needs the same disc_find_file/disc_read_sector pair and the same bounds.
#include "disc.h"
#include "disc_file.h"
#include "fs_util.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <lucent/log.h>
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

extern "C" int disc_extract_file(DiscState *d, const char *iso_path, const char *out_path) {
  std::vector<uint8_t> buf;
  if (!psx::cd::readDiscFile(*d, iso_path, buf)) {
    lucent::info("disc", "extract: {} not readable on disc", iso_path ? iso_path : "(null)");
    return 0;
  }
  if (!Fs::writeFile(out_path, buf.data(), buf.size())) {
    lucent::error("disc", "extract: failed to write {}", out_path ? out_path : "(null)");
    return 0;
  }
  lucent::info("disc",
               "extracted {} -> {} ({} bytes)",
               iso_path ? iso_path : "(null)",
               out_path ? out_path : "(null)",
               buf.size());
  return 1;
}

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

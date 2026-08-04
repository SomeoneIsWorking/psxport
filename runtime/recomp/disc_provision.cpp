// disc_provision.cpp — C-callable self-provisioning entry points for the disc backend (disc.c),
// implemented in C++ against Fs (game/core/fs_util.h) so host filesystem I/O goes through
// std::filesystem/fstream instead of hand-rolled FILE*/mkdir/dirent (USER directive 2026-07-14).
#include "disc.h"
#include "fs_util.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include "cfg.h"
#include <lucent/log.h>

extern "C" int disc_extract_file(DiscState* d, const char* iso_path, const char* out_path) {
  uint32_t lba, size;
  if (!disc_find_file(d, iso_path, &lba, &size)) {
    lucent::info("disc", "extract: {} not found on disc", iso_path ? iso_path : "(null)");
    return 0;
  }
  std::vector<uint8_t> buf(size);
  uint8_t sec[2048];
  for (uint32_t off = 0; off < size; off += 2048u, lba++) {
    if (!disc_read_sector(d, lba, sec)) {
      lucent::error("disc", "extract: sector read failed for {}", iso_path ? iso_path : "(null)");
      return 0;
    }
    uint32_t n = size - off < 2048u ? size - off : 2048u;
    memcpy(buf.data() + off, sec, n);
  }
  if (!Fs::writeFile(out_path, buf.data(), buf.size())) {
    lucent::error("disc", "extract: failed to write {}", out_path ? out_path : "(null)");
    return 0;
  }
  lucent::info("disc", "extracted {} -> {} ({} bytes)", iso_path ? iso_path : "(null)", out_path ? out_path : "(null)", size);
  return 1;
}

extern "C" int disc_dropin_scan(char* out, unsigned out_cap) {
  std::string found = Fs::findFirstWithExtension(".", ".chd");
  if (found.empty()) return 0;
  if (found.size() + 1 > out_cap) return 0;
  memcpy(out, found.c_str(), found.size() + 1);
  return 1;
}

// disc_provision.cpp — C-callable self-provisioning entry points for the disc backend (disc.c),
// implemented in C++ against Fs (game/core/fs_util.h) so host filesystem I/O goes through
// std::filesystem/fstream instead of hand-rolled FILE*/mkdir/dirent (USER directive 2026-07-14).
#include "disc.h"
#include "fs_util.h"
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" int disc_extract_file(DiscState* d, const char* iso_path, const char* out_path) {
  uint32_t lba, size;
  if (!disc_find_file(d, iso_path, &lba, &size)) {
    fprintf(stderr, "[disc] extract: %s not found on disc\n", iso_path);
    return 0;
  }
  std::vector<uint8_t> buf(size);
  uint8_t sec[2048];
  for (uint32_t off = 0; off < size; off += 2048u, lba++) {
    if (!disc_read_sector(d, lba, sec)) {
      fprintf(stderr, "[disc] extract: sector read failed for %s\n", iso_path);
      return 0;
    }
    uint32_t n = size - off < 2048u ? size - off : 2048u;
    memcpy(buf.data() + off, sec, n);
  }
  if (!Fs::writeFile(out_path, buf.data(), buf.size())) {
    fprintf(stderr, "[disc] extract: failed to write %s\n", out_path);
    return 0;
  }
  fprintf(stderr, "[disc] extracted %s -> %s (%u bytes)\n", iso_path, out_path, size);
  return 1;
}

extern "C" int disc_dropin_scan(char* out, unsigned out_cap) {
  std::string found = Fs::findFirstWithExtension(".", ".chd");
  if (found.empty()) return 0;
  if (found.size() + 1 > out_cap) return 0;
  memcpy(out, found.c_str(), found.size() + 1);
  return 1;
}

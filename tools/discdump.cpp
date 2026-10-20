// discdump: list a PSX CHD disc, extract one file, dump XA subheaders, or extract SYSTEM.CNF and the boot executable.
// Usage: discdump [disc.chd] [outdir]   (disc: the argument, else PSXPORT_DISC or a *.chd drop-in)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "cd_position.h"
#include "disc.h"

namespace fs = std::filesystem;

namespace {

using psx::cd::Disc;
using psx::cd::DiscFile;

uint32_t ReadLE32(const uint8_t *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}

std::string NormalizeName(std::string n) {
  if (auto sc = n.find(';'); sc != std::string::npos) {
    n.resize(sc);
  }
  for (char &c : n) {
    c = (char)std::toupper((unsigned char)c);
  }
  return n;
}

bool DumpFile(Disc &disc, const DiscFile &f, const fs::path &outpath, std::vector<uint8_t> *contents = nullptr) {
  std::vector<uint8_t> buf(((size_t)f.size + psx::cd::kDataSectorBytes - 1) / psx::cd::kDataSectorBytes *
                           psx::cd::kDataSectorBytes);
  for (uint32_t i = 0; i < buf.size() / psx::cd::kDataSectorBytes; i++) {
    if (!disc.readSector(f.lba + i, buf.data() + (size_t)i * psx::cd::kDataSectorBytes)) {
      return false;
    }
  }
  buf.resize(f.size);
  std::ofstream out(outpath, std::ios::binary);
  out.write((const char *)buf.data(), (std::streamsize)buf.size());
  if (contents) {
    *contents = std::move(buf);
  }
  return out.good();
}

std::optional<DiscFile> RootDir(Disc &disc) {
  uint8_t sec[psx::cd::kDataSectorBytes];
  if (!disc.readSector(16, sec) || std::memcmp(sec + 1, "CD001", 5) != 0) {
    std::fprintf(stderr, "no ISO9660 PVD at LBA 16\n");
    return std::nullopt;
  }
  const uint8_t *root = sec + 156;
  return DiscFile{ReadLE32(root + 2), ReadLE32(root + 10)};
}

// Recursively print the ISO9660 directory tree (files: full path, LBA, size).
void ListTree(Disc &disc, DiscFile dir, const std::string &prefix) {
  const uint32_t nsec = (dir.size + psx::cd::kDataSectorBytes - 1) / psx::cd::kDataSectorBytes;
  std::vector<uint8_t> buf((size_t)nsec * psx::cd::kDataSectorBytes);
  for (uint32_t i = 0; i < nsec; i++) {
    if (!disc.readSector(dir.lba + i, buf.data() + (size_t)i * psx::cd::kDataSectorBytes)) {
      return;
    }
  }

  // Directory records never straddle a 2048-byte sector boundary.
  for (uint32_t s = 0; s < buf.size(); s += psx::cd::kDataSectorBytes) {
    uint32_t pos = s;
    while (pos < s + psx::cd::kDataSectorBytes) {
      const uint8_t len = buf[pos];
      if (len == 0) {
        break; // remainder of this sector is padding
      }
      const uint8_t flags = buf[pos + 25];
      const uint8_t nlen = buf[pos + 32];
      const uint32_t e_lba = ReadLE32(&buf[pos + 2]);
      const uint32_t e_size = ReadLE32(&buf[pos + 10]);
      std::string name((const char *)&buf[pos + 33], nlen);
      pos += len;
      if (nlen == 1 && (name[0] == 0 || name[0] == 1)) {
        continue; // "." and ".."
      }
      const std::string disp = NormalizeName(name);
      if (flags & 0x02) // directory
      {
        std::printf("%s%s/\n", prefix.c_str(), disp.c_str());
        ListTree(disc, {e_lba, e_size}, prefix + disp + "/");
      } else {
        std::printf("%s%-16s  LBA %-7u  %u bytes\n", prefix.c_str(), disp.c_str(), e_lba, e_size);
      }
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  // `discdump list [disc.chd]`          prints the full ISO9660 file tree.
  // `discdump get <NAME> [disc] [out]`  extracts one root-dir file by name.
  const std::string mode = argc > 1 ? argv[1] : "";
  const bool list_mode = mode == "list";
  const bool get_mode = mode == "get";
  const bool subhdr_mode = mode == "subhdr"; // subhdr <lba> <count> [disc]: dump XA file/chan/submode
  const std::string get_name = (get_mode && argc > 2) ? argv[2] : "";
  const int disc_argi = list_mode ? 2 : (get_mode ? 3 : (subhdr_mode ? 4 : 1));
  // In extract mode an optional outdir follows the disc arg; list mode has no outdir.
  const int outdir_argi = disc_argi + 1;
  const fs::path outdir = (!list_mode && argc > outdir_argi) ? argv[outdir_argi] : fs::path("scratch/bin");
  if (!list_mode) {
    fs::create_directories(outdir);
  }

  Disc disc;
  if (!(argc > disc_argi ? disc.openImage(argv[disc_argi]) : disc.open())) {
    return 1;
  }

  if (subhdr_mode) {
    const uint32_t lba0 = argc > 2 ? (uint32_t)std::strtoul(argv[2], nullptr, 0) : 0;
    const uint32_t cnt = argc > 3 ? (uint32_t)std::strtoul(argv[3], nullptr, 0) : 16;
    std::printf("LBA      mode file chan submode  audio eof\n");
    for (uint32_t i = 0; i < cnt; i++) {
      uint8_t raw[psx::cd::kRawSectorBytes];
      if (!disc.readRaw(lba0 + i, raw, sizeof raw)) {
        std::printf("%-8u  <read fail>\n", lba0 + i);
        break;
      }
      const psx::cd::SectorSubheader header = psx::cd::sectorSubheader(raw);
      std::printf("%-8u  %2u  %3u  %3u   0x%02X     %d    %d\n",
                  lba0 + i,
                  raw[15],
                  header.file,
                  header.channel,
                  header.submode,
                  header.audio(),
                  header.endOfFile());
    }
    return 0;
  }

  if (list_mode) {
    const std::optional<DiscFile> root = RootDir(disc);
    if (!root) {
      return 1;
    }
    std::printf("root dir LBA %u, %u bytes\n\n", root->lba, root->size);
    ListTree(disc, *root, "");
    return 0;
  }

  if (get_mode) {
    if (get_name.empty()) {
      std::fprintf(stderr, "usage: discdump get <NAME> [disc] [outdir]\n");
      return 1;
    }
    const std::optional<DiscFile> f = disc.findFile(get_name.c_str());
    if (!f) {
      std::fprintf(stderr, "%s not found\n", get_name.c_str());
      return 1;
    }
    const std::string base = get_name.substr(get_name.find_last_of('/') + 1);
    const fs::path out = outdir / NormalizeName(base);
    if (!DumpFile(disc, *f, out)) {
      return 1;
    }
    std::printf("dumped %s (%u bytes, LBA %u)\n", out.c_str(), f->size, f->lba);
    return 0;
  }

  const std::optional<DiscFile> cnf = disc.findFile("SYSTEM.CNF");
  if (!cnf) {
    std::fprintf(stderr, "SYSTEM.CNF not found\n");
    return 1;
  }
  std::vector<uint8_t> cnf_data;
  if (!DumpFile(disc, *cnf, outdir / "SYSTEM.CNF", &cnf_data)) {
    return 1;
  }

  // Parse "BOOT = cdrom:\SCUS_xxx.yy;1"
  std::string cnf_text((const char *)cnf_data.data(), cnf_data.size());
  std::string boot_name;
  if (auto b = cnf_text.find("BOOT"); b != std::string::npos) {
    const auto eol = cnf_text.find_first_of("\r\n", b);
    const auto eq = cnf_text.find('=', b);
    std::string rest = cnf_text.substr(eq + 1, eol - eq - 1);
    if (auto sep = rest.find_last_of("\\:"); sep != std::string::npos) {
      rest = rest.substr(sep + 1);
    }
    const auto first = rest.find_first_not_of(" \t");
    const auto last = rest.find_last_not_of(" \t\0");
    if (first != std::string::npos) {
      boot_name = rest.substr(first, last - first + 1);
    }
  }
  if (boot_name.empty()) {
    std::fprintf(stderr, "could not parse BOOT from SYSTEM.CNF:\n%s\n", cnf_text.c_str());
    return 1;
  }
  std::printf("boot executable: %s\n", boot_name.c_str());

  const std::optional<DiscFile> exe = disc.findFile(boot_name.c_str());
  if (!exe) {
    std::fprintf(stderr, "boot executable not found in root dir\n");
    return 1;
  }
  const fs::path exe_out = outdir / NormalizeName(boot_name);
  if (!DumpFile(disc, *exe, exe_out)) {
    return 1;
  }
  std::printf("dumped %s (%u bytes, LBA %u)\n", exe_out.c_str(), exe->size, exe->lba);
  return 0;
}

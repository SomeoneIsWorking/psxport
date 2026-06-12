// discdump: extract SYSTEM.CNF and the boot executable from a PSX CHD disc image.
// Usage: discdump [disc.chd] [outdir]   (disc resolved per common/env.h if omitted)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <libchdr/chd.h>

#include "common/env.h"

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kRawFrameSize = 2448;  // 2352 raw + 96 subcode, CHD CD unit
constexpr uint32_t kUserDataSize = 2048;

class ChdDisc
{
public:
  bool Open(const std::string& path)
  {
    if (chd_open(path.c_str(), CHD_OPEN_READ, nullptr, &m_chd) != CHDERR_NONE)
      return false;
    const chd_header* h = chd_get_header(m_chd);
    m_hunk_bytes = h->hunkbytes;
    m_frames_per_hunk = h->hunkbytes / kRawFrameSize;
    m_hunk_count = h->totalhunks;
    m_hunk_buf.resize(m_hunk_bytes);
    return m_frames_per_hunk > 0;
  }

  ~ChdDisc()
  {
    if (m_chd)
      chd_close(m_chd);
  }

  // Read the 2048-byte user data of one sector (mode1 or mode2 form1).
  bool ReadSector(uint32_t lba, uint8_t* out)
  {
    const uint32_t hunk = lba / m_frames_per_hunk;
    const uint32_t offset = (lba % m_frames_per_hunk) * kRawFrameSize;
    if (hunk >= m_hunk_count)
      return false;
    if (hunk != m_cached_hunk)
    {
      if (chd_read(m_chd, hunk, m_hunk_buf.data()) != CHDERR_NONE)
        return false;
      m_cached_hunk = hunk;
    }
    const uint8_t* raw = m_hunk_buf.data() + offset;
    const uint8_t mode = raw[15];
    const uint32_t data_off = (mode == 2) ? 24 : 16;
    std::memcpy(out, raw + data_off, kUserDataSize);
    return true;
  }

private:
  chd_file* m_chd = nullptr;
  uint32_t m_hunk_bytes = 0;
  uint32_t m_frames_per_hunk = 0;
  uint32_t m_hunk_count = 0;
  uint32_t m_cached_hunk = UINT32_MAX;
  std::vector<uint8_t> m_hunk_buf;
};

uint32_t ReadLE32(const uint8_t* p)
{
  return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}

struct IsoFile
{
  uint32_t lba;
  uint32_t size;
};

std::string NormalizeName(std::string n)
{
  if (auto sc = n.find(';'); sc != std::string::npos)
    n.resize(sc);
  for (char& c : n)
    c = (char)std::toupper((unsigned char)c);
  return n;
}

// Find a file by name in the root directory.
bool FindRootFile(ChdDisc& disc, const std::string& want, IsoFile* out)
{
  uint8_t sec[kUserDataSize];
  if (!disc.ReadSector(16, sec) || std::memcmp(sec + 1, "CD001", 5) != 0)
  {
    std::fprintf(stderr, "no ISO9660 PVD at LBA 16\n");
    return false;
  }
  const uint8_t* rootrec = sec + 156;
  const uint32_t root_lba = ReadLE32(rootrec + 2);
  const uint32_t root_size = ReadLE32(rootrec + 10);

  const std::string want_norm = NormalizeName(want);
  for (uint32_t off = 0; off < root_size; off += kUserDataSize)
  {
    if (!disc.ReadSector(root_lba + off / kUserDataSize, sec))
      return false;
    uint32_t pos = 0;
    while (pos < kUserDataSize)
    {
      const uint8_t len = sec[pos];
      if (len == 0)
        break;  // rest of sector is padding
      const uint8_t name_len = sec[pos + 32];
      std::string name((const char*)sec + pos + 33, name_len);
      if (NormalizeName(name) == want_norm)
      {
        out->lba = ReadLE32(sec + pos + 2);
        out->size = ReadLE32(sec + pos + 10);
        return true;
      }
      pos += len;
    }
  }
  return false;
}

bool DumpFile(ChdDisc& disc, const IsoFile& f, const fs::path& outpath,
              std::vector<uint8_t>* contents = nullptr)
{
  std::vector<uint8_t> buf(((size_t)f.size + kUserDataSize - 1) / kUserDataSize *
                           kUserDataSize);
  for (uint32_t i = 0; i < buf.size() / kUserDataSize; i++)
    if (!disc.ReadSector(f.lba + i, buf.data() + (size_t)i * kUserDataSize))
      return false;
  buf.resize(f.size);
  std::ofstream out(outpath, std::ios::binary);
  out.write((const char*)buf.data(), (std::streamsize)buf.size());
  if (contents)
    *contents = std::move(buf);
  return out.good();
}

}  // namespace

int main(int argc, char** argv)
{
  const auto disc_path = psxport::ResolveDiscPath(argc > 1 ? argv[1] : "");
  if (!disc_path)
  {
    std::fprintf(stderr, "no disc image (arg, PSXPORT_DISC, or *.chd drop-in)\n");
    return 1;
  }
  const fs::path outdir = argc > 2 ? argv[2] : "scratch/bin";
  fs::create_directories(outdir);

  ChdDisc disc;
  if (!disc.Open(*disc_path))
  {
    std::fprintf(stderr, "failed to open CHD: %s\n", disc_path->c_str());
    return 1;
  }
  std::printf("disc: %s\n", disc_path->c_str());

  IsoFile cnf;
  if (!FindRootFile(disc, "SYSTEM.CNF", &cnf))
  {
    std::fprintf(stderr, "SYSTEM.CNF not found\n");
    return 1;
  }
  std::vector<uint8_t> cnf_data;
  if (!DumpFile(disc, cnf, outdir / "SYSTEM.CNF", &cnf_data))
    return 1;

  // Parse "BOOT = cdrom:\SCUS_xxx.yy;1"
  std::string cnf_text((const char*)cnf_data.data(), cnf_data.size());
  std::string boot_name;
  if (auto b = cnf_text.find("BOOT"); b != std::string::npos)
  {
    const auto eol = cnf_text.find_first_of("\r\n", b);
    const auto eq = cnf_text.find('=', b);
    std::string rest = cnf_text.substr(eq + 1, eol - eq - 1);
    if (auto sep = rest.find_last_of("\\:"); sep != std::string::npos)
      rest = rest.substr(sep + 1);
    const auto first = rest.find_first_not_of(" \t");
    const auto last = rest.find_last_not_of(" \t\0");
    if (first != std::string::npos)
      boot_name = rest.substr(first, last - first + 1);
  }
  if (boot_name.empty())
  {
    std::fprintf(stderr, "could not parse BOOT from SYSTEM.CNF:\n%s\n", cnf_text.c_str());
    return 1;
  }
  std::printf("boot executable: %s\n", boot_name.c_str());

  IsoFile exe;
  if (!FindRootFile(disc, boot_name, &exe))
  {
    std::fprintf(stderr, "boot executable not found in root dir\n");
    return 1;
  }
  const fs::path exe_out = outdir / NormalizeName(boot_name);
  if (!DumpFile(disc, exe, exe_out))
    return 1;
  std::printf("dumped %s (%u bytes, LBA %u)\n", exe_out.c_str(), exe.size, exe.lba);
  return 0;
}

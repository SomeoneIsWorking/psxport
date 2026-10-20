// crt0_extract — dumps a PS-X EXE's crt0 scan and the plan Crt0Boot would apply, through the shipping scanCrt0.
//
//   crt0_extract <EXECUTABLE> [--entry 0xADDR]
//
// Exit 0 = complete group and plan, 1 = the scan or plan refused, 2 = not a loadable PS-X EXE.
#include "crt0_boot.h"
#include "crt0_scan.h"
#include "psx_exe_image.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

namespace {

struct LoadedText {
  std::uint32_t base = 0;
  std::span<const std::uint8_t> bytes;

  std::uint32_t read(std::uint32_t address) const {
    if (address < base || address - base + 4u > bytes.size()) {
      return 0;
    }
    const std::uint8_t *p = bytes.data() + (address - base);
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8u) |
           (static_cast<std::uint32_t>(p[2]) << 16u) | (static_cast<std::uint32_t>(p[3]) << 24u);
  }
};

void printField(const char *name, bool resolved, std::uint32_t value) {
  if (resolved) {
    std::printf("    %-14s 0x%08X\n", name, value);
  } else {
    std::printf("    %-14s UNRESOLVED\n", name);
  }
}

int extract(std::span<const std::uint8_t> file, const char *label, std::uint32_t entryOverride) {
  const psx::cpu::PsxExeParseResult parsed = psx::cpu::parsePsxExeImage(file);
  if (!parsed) {
    std::fprintf(stderr, "crt0_extract: %s: %s\n", label, parsed.detail.c_str());
    return 2;
  }
  const psx::cpu::PsxExeImage &header = *parsed.image;
  const LoadedText text{header.textAddress, file.subspan(psx::cpu::kPsxExeHeaderBytes, header.textBytes)};
  const std::uint32_t entry = entryOverride != 0u ? entryOverride : header.entry;
  std::printf("crt0_extract %s\n  PS-X EXE header: pc0=0x%08X gp0=0x%08X text=0x%08X..0x%08X\n",
              label,
              header.entry,
              header.globalPointer,
              header.textAddress,
              header.textAddress + header.textBytes);

  const psx::game::Crt0Scan scan = psx::game::scanCrt0(entry, [&text](std::uint32_t address) {
    return text.read(address);
  });
  std::printf("  scan from 0x%08X: %d instruction(s) decoded, stopped on \"%s\", %d zero word(s)\n  boot group:\n",
              entry,
              scan.decoded,
              scan.stopped,
              scan.allZero);
  printField("bssZeroLo", scan.haveBssLo, scan.bssLo);
  printField("bssZeroHi", scan.haveBssHi, scan.bssHi);
  printField("stackTopBase", scan.haveStackTop, scan.stackTopBase);
  if (scan.haveBias) {
    std::printf("    %-14s %d\n", "stackBias", scan.bias);
  } else {
    std::printf("    %-14s UNRESOLVED\n", "stackBias");
  }
  printField("stackTopBase2", scan.haveReserve, scan.stackTopBase2);
  printField("heapBase", scan.haveHeapBase, scan.heapBase);
  printField("gp", scan.haveGp, scan.gp);
  printField("libcInit", scan.haveLibcInit, scan.libcInit);
  printField("heapSizePtr", scan.scanComplete, scan.heapSizePtr);
  printField("heapBasePtr", scan.scanComplete, scan.heapBasePtr);
  std::printf("  libcInit is the A(39h) InitHeap BIOS thunk: %s · a1 live at the call: %s · delay slot is addi "
              "a0,a0,4: %s\n",
              scan.libcInitIsInitHeap ? "YES" : "no",
              scan.a1LiveAtCall ? "YES" : "no",
              scan.a0PlusFour ? "YES" : "no");

  const psx::game::Crt0Plan plan =
      psx::game::planCrt0(scan, text.read(scan.stackTopBase), text.read(scan.stackTopBase2), "crt0_extract");
  if (!plan.ok) {
    return 1;
  }
  std::printf("  crt0_plan (THE shipping arithmetic, run on this image): sp=0x%08X gp=0x%08X"
              " InitHeap(a0=0x%08X, a1=0x%X)\n",
              plan.sp,
              plan.gp,
              plan.a0,
              plan.a1);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  const char *path = nullptr;
  std::uint32_t entryOverride = 0;
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--entry") == 0 && i + 1 < argc) {
      entryOverride = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0));
    } else if (path == nullptr) {
      path = argv[i];
    }
  }
  if (path == nullptr) {
    std::fprintf(stderr, "usage: crt0_extract <EXECUTABLE> [--entry 0xADDR]\n");
    return 2;
  }
  std::FILE *f = std::fopen(path, "rb");
  if (f == nullptr) {
    std::fprintf(stderr, "crt0_extract: cannot open %s\n", path);
    return 2;
  }
  std::vector<std::uint8_t> bytes;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size > 0) {
    bytes.resize(static_cast<std::size_t>(size));
    if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
      bytes.clear();
    }
  }
  std::fclose(f);
  return extract(bytes, path, entryOverride);
}

// File-based PS-X EXE startup. Structural mapping belongs to psx_exe_image;
// callers needing exact revision authentication must authenticate and map the
// same buffer through loadPsxExeImage instead of reopening a validated path.
#include "psx_exe_image.h"

#include "core.h"

#include <cstdlib>
#include <fstream>
#include <lucent/log.h>
#include <vector>

namespace psx::cpu {

void applyPsxExeTopLevelRegisters(Core &core, const PsxExeImage &image) {
  if (image.stackBase == 0) {
    core.r[29] = 0x801ffff0u;
    core.r[30] = core.r[29];
  }
  core.r[31] = 0xdead0000u; // top-level return sentinel
}

} // namespace psx::cpu

void load_exe(const char *path, Core *core) {
  if (!path || !core) {
    lucent::error("boot", "PS-X EXE startup requires a path and Core");
    std::exit(1);
  }
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    lucent::error("boot", "cannot open PS-X EXE '{}'", path);
    std::exit(1);
  }
  const auto size = file.tellg();
  if (size < static_cast<std::streamoff>(psx::cpu::kPsxExeHeaderBytes) ||
      size > static_cast<std::streamoff>(psx::cpu::kPsxExeMaxBytes)) {
    lucent::error("boot", "PS-X EXE '{}' has an unreadable or out-of-bounds size", path);
    std::exit(1);
  }
  std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
  file.seekg(0);
  file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!file || file.peek() != std::ifstream::traits_type::eof()) {
    lucent::error("boot", "PS-X EXE '{}' changed size or could not be read completely", path);
    std::exit(1);
  }
  const auto loaded = psx::cpu::loadPsxExeImage(*core, bytes, path);
  if (!loaded) {
    lucent::error("boot", "cannot load PS-X EXE '{}': {}", path, loaded.detail);
    std::exit(1);
  }
  psx::cpu::applyPsxExeTopLevelRegisters(*core, loaded.image);
  lucent::info("boot",
               "loaded {}: entry 0x{:08X} load 0x{:08X} text 0x{:X} sp 0x{:08X}",
               path,
               loaded.image.entry,
               loaded.image.textAddress,
               loaded.image.textBytes,
               core->r[29]);
}

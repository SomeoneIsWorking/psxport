#include "bios_load_exec.h"

#include "core.h"
#include "disc_file.h"
#include "execution_control.h"
#include "execution_exit.h"
#include "psx_exe_image.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <lucent/log.h>

namespace psx::hle {
namespace {

// The BIOS's own failure answer for a load it could not perform.
constexpr std::uint32_t kLoadFailed = 0xffffffffu;

// A guest path ("cdrom:\DIR\FILE.EXT;1") is read with this bound.
constexpr std::size_t kFilenameBytes = 256;

// "device:path" to the ISO9660 path; the disc lookup already ignores case, ";version" and leading separators.
std::string isoPathFromBiosPath(const char *biosPath) {
  std::string path = biosPath;
  const std::size_t colon = path.find(':');
  if (colon != std::string::npos) {
    path.erase(0, colon + 1);
  }
  std::replace(path.begin(), path.end(), '/', '\\');
  return path;
}

// The BIOS's failure answer; the caller resumes at its own $ra.
bool refuseLoad(Core &core, const std::string &reason) {
  lucent::error("hle", "LoadExec refused: {}", reason);
  core.r[psx::cpu::kV0] = kLoadFailed;
  return true;
}

} // namespace

DiscFileReader discFileReader(psx::cd::Disc &disc) {
  return [&disc](const char *isoPath, std::vector<std::uint8_t> &out) {
    return psx::cd::readDiscFile(disc, isoPath, out);
  };
}

bool dispatchLoadExec(Core &core, const DiscFileReader &readFile) {
  char filename[kFilenameBytes];
  core.readCString(core.r[psx::cpu::kA0], filename, sizeof filename);
  const std::size_t length = std::strlen(filename);
  if (length == 0u) {
    return refuseLoad(core,
                      lucent::format("the filename pointer 0x{:08X} is empty or unreadable", core.r[psx::cpu::kA0]));
  }
  // A name that filled the buffer was cut.
  if (core.mem_r8(core.r[psx::cpu::kA0] + static_cast<std::uint32_t>(length)) != 0u) {
    return refuseLoad(
        core, lucent::format("the filename at 0x{:08X} exceeds {} bytes", core.r[psx::cpu::kA0], kFilenameBytes - 1u));
  }

  // A stated a1/a2 stack replaces the header's; it is checked before any byte moves.
  const std::uint32_t statedStackBase = core.r[psx::cpu::kA1];
  const std::uint64_t statedStack =
      static_cast<std::uint64_t>(statedStackBase) + static_cast<std::uint64_t>(core.r[psx::cpu::kA2]);
  if (statedStackBase != 0u && (statedStack > std::numeric_limits<std::uint32_t>::max() ||
                                !core.isGuestStorage(static_cast<std::uint32_t>(statedStack), sizeof(std::uint32_t)))) {
    return refuseLoad(core,
                      lucent::format("its stack (base 0x{:08X}, offset 0x{:X}) is not guest memory",
                                     statedStackBase,
                                     core.r[psx::cpu::kA2]));
  }

  std::vector<std::uint8_t> bytes;
  const std::string isoPath = isoPathFromBiosPath(filename);
  if (!readFile) {
    return refuseLoad(core, lucent::format("no CD device is bound to this Core, so '{}' cannot be read", filename));
  }
  if (!readFile(isoPath.c_str(), bytes)) {
    return refuseLoad(core, lucent::format("'{}' is not readable from the disc as {}", filename, isoPath));
  }

  const psx::cpu::PsxExeLoadResult loaded = psx::cpu::loadTopLevelPsxExe(core, bytes, filename);
  if (!loaded) {
    return refuseLoad(core, lucent::format("'{}': {}", filename, loaded.detail));
  }
  if (statedStackBase != 0u) {
    core.r[psx::cpu::kSp] = core.r[psx::cpu::kFp] = static_cast<std::uint32_t>(statedStack);
  }

  core.r[psx::cpu::kV0] = 0;
  lucent::info("hle",
               "LoadExec '{}': loaded at 0x{:08X}, entry 0x{:08X}, gp 0x{:08X}, sp 0x{:08X}",
               filename,
               loaded.image.textAddress,
               loaded.image.entry,
               core.r[psx::cpu::kGp],
               core.r[psx::cpu::kSp]);
  // The BIOS does not return on success: the guest continues at the loaded entry within this turn.
  psx::cpu::requestExecutionExit(
      core, psx::cpu::ExecutionResult{psx::cpu::ExecutionExitReason::GuestReturn, loaded.image.entry, 0});
  return true;
}

} // namespace psx::hle
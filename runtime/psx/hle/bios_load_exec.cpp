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

// MIPS o32 argument registers (== core.r[]).
enum { R_A0 = 4, R_A1 = 5, R_A2 = 6, R_V0 = 2 };

// The BIOS's own failure answer for a load it could not perform.
constexpr std::uint32_t kLoadFailed = 0xffffffffu;

// A PSX path is "cdrom:\DIR\FILE.EXT;1", so a device name plus one directory and a file name is well
// inside this; the bound exists so a guest string is never read without one.
constexpr std::size_t kFilenameBytes = 256;

// nocash psx-spx spells a BIOS path "device:path". `disc_find_file` resolves an absolute ISO9660
// path and already ignores case, ";version" and leading separators, so the only translation this
// service owes is dropping the DEVICE name the guest handed the BIOS and using one separator.
std::string isoPathFromBiosPath(const char *biosPath) {
  std::string path = biosPath;
  const std::size_t colon = path.find(':');
  if (colon != std::string::npos) {
    path.erase(0, colon + 1);
  }
  std::replace(path.begin(), path.end(), '/', '\\');
  return path;
}

// Answer the BIOS's failure, in one place, with the reason that produced it. The caller resumes at
// its own `r[31]`: this is the branch the BIOS returns from.
bool refuseLoad(Core &core, const std::string &reason) {
  lucent::error("hle", "LoadExec refused: {}", reason);
  core.r[R_V0] = kLoadFailed;
  return true;
}

} // namespace

DiscFileReader discFileReader(DiscState &disc) {
  return [&disc](const char *isoPath, std::vector<std::uint8_t> &out) {
    return psx::cd::readDiscFile(disc, isoPath, out);
  };
}

bool dispatchLoadExec(Core &core, const DiscFileReader &readFile) {
  char filename[kFilenameBytes];
  core.readCString(core.r[R_A0], filename, sizeof filename);
  const std::size_t length = std::strlen(filename);
  if (length == 0u) {
    return refuseLoad(core, lucent::format("the filename pointer 0x{:08X} is empty or unreadable", core.r[R_A0]));
  }
  // A filename that filled the buffer was CUT, and admitting the truncated name would load a
  // different file than the guest asked for. Refused rather than guessed at.
  if (core.mem_r8(core.r[R_A0] + static_cast<std::uint32_t>(length)) != 0u) {
    return refuseLoad(core,
                      lucent::format("the filename at 0x{:08X} exceeds {} bytes", core.r[R_A0], kFilenameBytes - 1u));
  }

  // THE CALLER OWNS THE STACK. That is what a1/a2 are for: a stated (stackbase, stackoffset) replaces
  // whatever the loaded header declares, and an unstated base leaves the header's own stack — or the
  // conventional boot stack — standing.
  //
  // RESOLVED BEFORE ANY BYTES MOVE, and refused here rather than after admission.
  // `loadPsxExeImage` publishes the payload and rewrites PC/GP/SP, so a refusal that ran after it
  // would answer -1 with half a program in RAM and a new image identity resident over it: a state the
  // guest cannot make sense of and can only escape by the run ending. Refusing first leaves the
  // machine exactly as the caller found it.
  const std::uint32_t statedStackBase = core.r[R_A1];
  const std::uint64_t statedStack =
      static_cast<std::uint64_t>(statedStackBase) + static_cast<std::uint64_t>(core.r[R_A2]);
  if (statedStackBase != 0u && (statedStack > std::numeric_limits<std::uint32_t>::max() ||
                                !core.isGuestStorage(static_cast<std::uint32_t>(statedStack), sizeof(std::uint32_t)))) {
    return refuseLoad(
        core,
        lucent::format("its stack (base 0x{:08X}, offset 0x{:X}) is not guest memory", statedStackBase, core.r[R_A2]));
  }

  std::vector<std::uint8_t> bytes;
  const std::string isoPath = isoPathFromBiosPath(filename);
  if (!readFile) {
    return refuseLoad(core, lucent::format("no CD device is bound to this Core, so '{}' cannot be read", filename));
  }
  if (!readFile(isoPath.c_str(), bytes)) {
    return refuseLoad(core, lucent::format("'{}' is not readable from the disc as {}", filename, isoPath));
  }

  // The ONE executable-admission owner: it validates the header, publishes the payload as one
  // executable write (so no stale translation survives), activates the image identity that owns the
  // range, and establishes PC/GP and the header's own stack.
  const psx::cpu::PsxExeLoadResult loaded = psx::cpu::loadPsxExeImage(core, bytes, filename);
  if (!loaded) {
    return refuseLoad(core, lucent::format("'{}': {}", filename, loaded.detail));
  }
  psx::cpu::applyPsxExeTopLevelRegisters(core, loaded.image);
  if (statedStackBase != 0u) {
    core.r[29] = core.r[30] = static_cast<std::uint32_t>(statedStack);
  }

  core.r[R_V0] = 0;
  lucent::info("hle",
               "LoadExec '{}': loaded at 0x{:08X}, entry 0x{:08X}, gp 0x{:08X}, sp 0x{:08X}",
               filename,
               loaded.image.textAddress,
               loaded.image.entry,
               core.r[28],
               core.r[29]);
  // START IT. The BIOS does not return to its caller on success, so the guest's continuation is the
  // loaded program's entry and NOT this leaf's r[31]. A requested exit carrying that address is the
  // framework's own contract for "resume where this leaf chose" (execution_control.h), and
  // GuestReturn is the reason that keeps the executor inside this turn and resumes at the stated
  // address instead of handing control back to the title's frame driver.
  psx::cpu::requestExecutionExit(
      core,
      psx::cpu::ExecutionResult{
          psx::cpu::ExecutionExitReason::GuestReturn, loaded.image.entry, 0, "LoadExec: " + std::string(filename)});
  return true;
}

} // namespace psx::hle
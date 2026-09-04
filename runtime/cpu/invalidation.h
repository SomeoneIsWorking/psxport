#pragma once

#include "guest_program_image.h"

#include <cstdint>

class Core;

namespace psx::cpu {

enum class ExecutableWriteSource : std::uint8_t {
  Cpu,
  Dma,
  ModuleLoad,
  Debugger,
  Savestate,
  Native,
};

void notifyExecutableWrite(Core &core, GuestAddressRange range, ExecutableWriteSource source);
void notifyExecutableStateReplaced(Core &core, ExecutableWriteSource source);

} // namespace psx::cpu

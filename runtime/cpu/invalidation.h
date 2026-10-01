#pragma once

#include "guest_program_image.h"

#include <cstddef>
#include <cstdint>

class Core;

namespace psx::cpu {

enum class ExecutableWriteSource : std::uint8_t {
  Cpu,
  MappedStore,
  Dma,
  ModuleLoad,
  Debugger,
  Savestate,
  Native,
};

inline constexpr std::size_t kExecutableWriteSourceCount = 7;

void notifyExecutableWrite(Core &core, GuestAddressRange range, ExecutableWriteSource source);
void notifyExecutableStateReplaced(Core &core, ExecutableWriteSource source);

} // namespace psx::cpu

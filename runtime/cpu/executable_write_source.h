#pragma once

#include <cstddef>
#include <cstdint>

namespace psx::cpu {

// Who wrote guest memory that translated code may cover; the invalidation owner counts by it.
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

} // namespace psx::cpu

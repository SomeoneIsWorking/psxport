#pragma once

#include <cstdint>

class Core;

namespace psx::cpu {

void accountGuestInstructions(Core &core, std::uint32_t instructions);
void servicePendingWork(Core &core);
enum class SyscallResult : std::uint8_t {
  Handled,
  UnsupportedSelector,
  MissingContext,
};

// Kernel selectors 0..2: no function, EnterCriticalSection, ExitCriticalSection.
inline constexpr std::uint32_t kExitCriticalSection = 2u;

SyscallResult handleSyscall(Core &core, std::uint32_t instructionPc);
void handleBreak(Core &core, std::uint32_t code);

} // namespace psx::cpu

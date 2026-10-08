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

SyscallResult handleSyscall(Core &core, std::uint32_t code, std::uint32_t instructionPc);
void handleBreak(Core &core, std::uint32_t code);

} // namespace psx::cpu

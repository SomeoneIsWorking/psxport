// The shipping syscall HLE must retain the exception record the independent CPU produces, even
// though the native runtime handles the kernel operation without executing a BIOS vector. Exercise
// the public runtime ABI so this test cannot pass against a test-only copy of the transition.
#include "execution_services.h"
#include "game.h"
#include "testutil.h"

#include <cstddef>
#include <cstring>

namespace {

constexpr uint32_t kSyscallPc = 0x8003E1FCu;
constexpr uint32_t kStatusUpper = 0x00600000u;
constexpr uint32_t kPendingInterruptCause = 0x00001200u;
constexpr uint32_t kSyscallCause = kPendingInterruptCause | 0x20u;
constexpr std::size_t kV0 = 2;
constexpr std::size_t kA0 = 4;

Game &testGame() {
  static Game game;
  return game;
}

Core &resetCore() {
  Game &game = testGame();
  Core &core = game.core;
  std::memset(static_cast<R3000 *>(&core), 0, sizeof(R3000));
  std::memset(core.cop0, 0, sizeof(core.cop0));
  game.hle.irq_enabled = 1;
  return core;
}

void test_enter_critical_retains_syscall_exception_and_disables_interrupts() {
  Core &core = resetCore();
  core.r[kA0] = 1;
  core.cop0[12] = kStatusUpper | 0x3Fu;
  core.cop0[13] = 0xABCD1234u;

  psx::cpu::handleSyscall(core, 0, kSyscallPc);

  CHECK_EQ(core.r[kV0], 1u);
  CHECK_EQ(core.game->hle.irq_enabled, 0);
  CHECK_EQ(core.cop0[12], kStatusUpper | 0x3Eu);
  CHECK_EQ(core.cop0[13], kSyscallCause);
  CHECK_EQ(core.cop0[14], kSyscallPc);
}

void test_exit_critical_returns_with_current_interrupt_enable_set() {
  Core &core = resetCore();
  core.game->hle.irq_enabled = 0;
  core.r[kA0] = 2;
  core.cop0[12] = kStatusUpper | 0x3Eu;
  core.cop0[13] = kPendingInterruptCause;

  psx::cpu::handleSyscall(core, 0, kSyscallPc + 0x10u);

  CHECK_EQ(core.r[kV0], 0u);
  CHECK_EQ(core.game->hle.irq_enabled, 1);
  CHECK_EQ(core.cop0[12], kStatusUpper | 0x3Fu);
  CHECK_EQ(core.cop0[13], kSyscallCause);
  CHECK_EQ(core.cop0[14], kSyscallPc + 0x10u);
}

void test_non_interrupt_kernel_op_returns_from_exactly_one_exception_level() {
  Core &core = resetCore();
  core.r[kA0] = 0;
  core.cop0[12] = kStatusUpper | 0x15u;

  psx::cpu::handleSyscall(core, 0x54321u, kSyscallPc + 0x20u);

  CHECK_EQ(core.r[kV0], 0u);
  CHECK_EQ(core.cop0[12], kStatusUpper | 0x15u);
  CHECK_EQ(core.cop0[13], 0x20u);
  CHECK_EQ(core.cop0[14], kSyscallPc + 0x20u);
}

} // namespace

int main() {
  RUN(enter_critical_retains_syscall_exception_and_disables_interrupts);
  RUN(exit_critical_returns_with_current_interrupt_enable_set);
  RUN(non_interrupt_kernel_op_returns_from_exactly_one_exception_level);
  return pt_summary();
}

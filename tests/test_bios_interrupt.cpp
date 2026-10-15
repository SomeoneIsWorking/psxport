// test_bios_interrupt.cpp — the BIOS custom exception exit is a saved CPU context, not a callback.
//
// B0:0x19 HookEntryInt receives a guest jmp_buf with the layout measured independently in Crash
// Bash and Mega Man X4. After the ordinary SysEnqIntRP chain has been walked, the BIOS restores this
// context, makes setjmp return non-zero, and jumps to the saved RA. That continuation is each game's
// master hardware dispatcher; treating the buffer as a function pointer bypasses the real IRQ table.
//
// This hermetic test pins both answers: a complete saved context is restored exactly, while a missing
// buffer or missing continuation is refused without changing the interrupted register file.
#include "bios_interrupt.h"
#include "game.h"
#include "r3000.h"
#include "stub_game_runtime.h"
#include "testutil.h"
#include <memory>

namespace {

constexpr uint32_t kBuffer = 0x80010000u;
constexpr uint32_t kResume = 0x80031AE8u;
int dispatch_entered = 0;
int dispatch_fell_through = 0;
Game *dispatch_game = nullptr;

void write_context(Core *c, uint32_t resume) {
  c->mem_w32(kBuffer + 0x00u, resume);
  c->mem_w32(kBuffer + 0x04u, 0x80111110u); // sp
  c->mem_w32(kBuffer + 0x08u, 0x80222220u); // fp
  for (uint32_t i = 0; i < 8; i++) {
    c->mem_w32(kBuffer + 0x0Cu + i * 4u, 0xA0000000u + i);
  }
  c->mem_w32(kBuffer + 0x2Cu, 0x80333330u); // gp
}

void poison_context_registers(Core *c) {
  c->r[psx::cpu::kV0] = 0xDEADBEEFu;
  c->r[psx::cpu::kSp] = 0xDEAD0001u;
  c->r[psx::cpu::kFp] = 0xDEAD0002u;
  c->r[psx::cpu::kGp] = 0xDEAD0003u;
  for (uint32_t i = 0; i < 8; i++) {
    c->r[psx::cpu::kS0 + i] = 0xDEAD1000u + i;
  }
}

void test_restores_the_measured_jmp_buf_layout(void) {
  psx::test::StubGameRuntime stubRuntime;
  const std::unique_ptr<Core> c = std::make_unique<Core>(stubRuntime);
  write_context(c.get(), kResume);
  poison_context_registers(c.get());

  CHECK_EQ(psx::hle::enterInterruptCustomExit(c.get(), kBuffer), kResume);
  CHECK_EQ(c->r[psx::cpu::kV0], 1u); // setjmp's second return is non-zero
  CHECK_EQ(c->r[psx::cpu::kSp], 0x80111110u);
  CHECK_EQ(c->r[psx::cpu::kFp], 0x80222220u);
  CHECK_EQ(c->r[psx::cpu::kGp], 0x80333330u);
  for (uint32_t i = 0; i < 8; i++) {
    CHECK_EQ(c->r[psx::cpu::kS0 + i], 0xA0000000u + i);
  }
}

void test_refuses_a_missing_buffer_without_clobbering(void) {
  psx::test::StubGameRuntime stubRuntime;
  const std::unique_ptr<Core> c = std::make_unique<Core>(stubRuntime);
  poison_context_registers(c.get());

  CHECK_EQ(psx::hle::enterInterruptCustomExit(c.get(), 0), 0u);
  CHECK_EQ(c->r[psx::cpu::kV0], 0xDEADBEEFu);
  CHECK_EQ(c->r[psx::cpu::kSp], 0xDEAD0001u);
  CHECK_EQ(c->r[psx::cpu::kFp], 0xDEAD0002u);
  CHECK_EQ(c->r[psx::cpu::kGp], 0xDEAD0003u);
  for (uint32_t i = 0; i < 8; i++) {
    CHECK_EQ(c->r[psx::cpu::kS0 + i], 0xDEAD1000u + i);
  }
}

void test_refuses_a_buffer_without_a_continuation(void) {
  psx::test::StubGameRuntime stubRuntime;
  const std::unique_ptr<Core> c = std::make_unique<Core>(stubRuntime);
  write_context(c.get(), 0);
  poison_context_registers(c.get());

  CHECK_EQ(psx::hle::enterInterruptCustomExit(c.get(), kBuffer), 0u);
  CHECK_EQ(c->r[psx::cpu::kV0], 0xDEADBEEFu);
  CHECK_EQ(c->r[psx::cpu::kSp], 0xDEAD0001u);
  CHECK_EQ(c->r[psx::cpu::kFp], 0xDEAD0002u);
  CHECK_EQ(c->r[psx::cpu::kGp], 0xDEAD0003u);
  for (uint32_t i = 0; i < 8; i++) {
    CHECK_EQ(c->r[psx::cpu::kS0 + i], 0xDEAD1000u + i);
  }
}

void test_bios_hook_reset_and_return_entry_points(void) {
  psx::test::StubGameRuntime stubRuntime;
  const std::unique_ptr<Game> game = std::make_unique<Game>(stubRuntime, psx::config::Config{});
  Core *c = &game->core;
  write_context(c, kResume);

  c->r[4] = kBuffer;
  c->r[psx::cpu::kV0] = 0xDEADBEEFu;
  CHECK(game->hle.dispatchBios('B', 0x19));
  CHECK_EQ(game->hle.exception_exit_buf, kBuffer);
  CHECK_EQ(c->r[psx::cpu::kV0], 0u);

  c->r[psx::cpu::kV0] = 0xDEADBEEFu;
  CHECK(game->hle.dispatchBios('B', 0x18));
  CHECK_EQ(c->r[psx::cpu::kV0], 0u);
  CHECK_EQ(game->hle.exception_exit_buf, 0u);
}

void return_from_exception_dispatch(Core *, uint32_t resume) {
  CHECK_EQ(resume, kResume);
  dispatch_entered++;
  dispatch_game->hle.dispatchBios('B', 0x17);
  dispatch_fell_through++; // unreachable: B0:0x17 is a non-returning exception return
}

void ordinary_return_dispatch(Core *, uint32_t resume) {
  CHECK_EQ(resume, kResume);
  dispatch_entered++;
  dispatch_fell_through++;
}

void test_return_from_exception_unwinds_instead_of_falling_through(void) {
  psx::test::StubGameRuntime stubRuntime;
  const std::unique_ptr<Game> game = std::make_unique<Game>(stubRuntime, psx::config::Config{});
  write_context(&game->core, kResume);
  dispatch_entered = 0;
  dispatch_fell_through = 0;
  dispatch_game = game.get();
  game->hle.custom_exit_active = 1;

  CHECK_EQ(psx::hle::dispatchInterruptCustomExit(&game->core, kBuffer, return_from_exception_dispatch),
           BiosInterruptDispatchResult::ReturnedFromException);
  game->hle.custom_exit_active = 0;
  dispatch_game = nullptr;
  CHECK_EQ(dispatch_entered, 1);
  CHECK_EQ(dispatch_fell_through, 0);
}

void test_custom_exit_reports_an_illegal_normal_return(void) {
  psx::test::StubGameRuntime stubRuntime;
  const std::unique_ptr<Core> c = std::make_unique<Core>(stubRuntime);
  write_context(c.get(), kResume);
  dispatch_entered = 0;
  dispatch_fell_through = 0;

  CHECK_EQ(psx::hle::dispatchInterruptCustomExit(c.get(), kBuffer, ordinary_return_dispatch),
           BiosInterruptDispatchResult::FellThrough);
  CHECK_EQ(dispatch_entered, 1);
  CHECK_EQ(dispatch_fell_through, 1);
}

// A nested ISR must not see the interrupted ra as its stop address: it can legitimately execute that pc.
void test_exception_stack_ra_is_a_sentinel_not_guest_code(void) {
  psx::test::StubGameRuntime stubRuntime;
  const std::unique_ptr<Core> c = std::make_unique<Core>(stubRuntime);
  c->r[psx::cpu::kSp] = 0x80001234u;
  c->r[31] = 0x80028CBCu;
  Hle::enterExceptionStack(*c);
  CHECK_EQ(c->r[psx::cpu::kSp], Hle::kExceptionStackTop);
  CHECK_EQ(c->r[31], Hle::kInterruptReturnSentinel);
}

} // namespace

int main(void) {
  RUN(restores_the_measured_jmp_buf_layout);
  RUN(refuses_a_missing_buffer_without_clobbering);
  RUN(refuses_a_buffer_without_a_continuation);
  RUN(bios_hook_reset_and_return_entry_points);
  RUN(return_from_exception_unwinds_instead_of_falling_through);
  RUN(custom_exit_reports_an_illegal_normal_return);
  RUN(exception_stack_ra_is_a_sentinel_not_guest_code);
  return psx::test::summary();
}

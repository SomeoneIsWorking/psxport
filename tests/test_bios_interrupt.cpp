// test_bios_interrupt.cpp — the BIOS custom exception exit is a saved CPU context, not a callback.
//
// B0:0x19 HookEntryInt receives a guest jmp_buf with the layout measured independently in Crash
// Bash and Mega Man X4. After the ordinary SysEnqIntRP chain has been walked, the BIOS restores this
// context, makes setjmp return non-zero, and jumps to the saved RA. That continuation is each game's
// master hardware dispatcher; treating the buffer as a function pointer bypasses the real IRQ table.
//
// This hermetic test pins both answers: a complete saved context is restored exactly, while a missing
// buffer or missing continuation is refused without changing the interrupted register file.
#include "../runtime/recomp/bios_interrupt.h"
#include "../runtime/recomp/game.h"
#include "testutil.h"
#include <memory>

enum { R_V0 = 2, R_S0 = 16, R_GP = 28, R_SP = 29, R_FP = 30 };

static constexpr uint32_t kBuffer = 0x80010000u;
static constexpr uint32_t kResume = 0x80031AE8u;
static int dispatch_entered = 0;
static int dispatch_fell_through = 0;
static Game *dispatch_game = nullptr;

static void write_context(Core *c, uint32_t resume) {
  c->mem_w32(kBuffer + 0x00u, resume);
  c->mem_w32(kBuffer + 0x04u, 0x80111110u); // sp
  c->mem_w32(kBuffer + 0x08u, 0x80222220u); // fp
  for (uint32_t i = 0; i < 8; i++) {
    c->mem_w32(kBuffer + 0x0Cu + i * 4u, 0xA0000000u + i);
  }
  c->mem_w32(kBuffer + 0x2Cu, 0x80333330u); // gp
}

static void poison_context_registers(Core *c) {
  c->r[R_V0] = 0xDEADBEEFu;
  c->r[R_SP] = 0xDEAD0001u;
  c->r[R_FP] = 0xDEAD0002u;
  c->r[R_GP] = 0xDEAD0003u;
  for (uint32_t i = 0; i < 8; i++) {
    c->r[R_S0 + i] = 0xDEAD1000u + i;
  }
}

static void test_restores_the_measured_jmp_buf_layout(void) {
  const std::unique_ptr<Core> c = std::make_unique<Core>();
  write_context(c.get(), kResume);
  poison_context_registers(c.get());

  CHECK_EQ(bios_interrupt_enter_custom_exit(c.get(), kBuffer), kResume);
  CHECK_EQ(c->r[R_V0], 1u); // setjmp's second return is non-zero
  CHECK_EQ(c->r[R_SP], 0x80111110u);
  CHECK_EQ(c->r[R_FP], 0x80222220u);
  CHECK_EQ(c->r[R_GP], 0x80333330u);
  for (uint32_t i = 0; i < 8; i++) {
    CHECK_EQ(c->r[R_S0 + i], 0xA0000000u + i);
  }
}

static void test_refuses_a_missing_buffer_without_clobbering(void) {
  const std::unique_ptr<Core> c = std::make_unique<Core>();
  poison_context_registers(c.get());

  CHECK_EQ(bios_interrupt_enter_custom_exit(c.get(), 0), 0u);
  CHECK_EQ(c->r[R_V0], 0xDEADBEEFu);
  CHECK_EQ(c->r[R_SP], 0xDEAD0001u);
  CHECK_EQ(c->r[R_FP], 0xDEAD0002u);
  CHECK_EQ(c->r[R_GP], 0xDEAD0003u);
  for (uint32_t i = 0; i < 8; i++) {
    CHECK_EQ(c->r[R_S0 + i], 0xDEAD1000u + i);
  }
}

static void test_refuses_a_buffer_without_a_continuation(void) {
  const std::unique_ptr<Core> c = std::make_unique<Core>();
  write_context(c.get(), 0);
  poison_context_registers(c.get());

  CHECK_EQ(bios_interrupt_enter_custom_exit(c.get(), kBuffer), 0u);
  CHECK_EQ(c->r[R_V0], 0xDEADBEEFu);
  CHECK_EQ(c->r[R_SP], 0xDEAD0001u);
  CHECK_EQ(c->r[R_FP], 0xDEAD0002u);
  CHECK_EQ(c->r[R_GP], 0xDEAD0003u);
  for (uint32_t i = 0; i < 8; i++) {
    CHECK_EQ(c->r[R_S0 + i], 0xDEAD1000u + i);
  }
}

static void test_bios_hook_reset_and_return_entry_points(void) {
  const std::unique_ptr<Game> game = std::make_unique<Game>();
  Core *c = &game->core;
  write_context(c, kResume);

  c->r[4] = kBuffer;
  c->r[R_V0] = 0xDEADBEEFu;
  CHECK(game->hle.dispatchBios('B', 0x19));
  CHECK_EQ(game->hle.exception_exit_buf, kBuffer);
  CHECK_EQ(c->r[R_V0], 0u);

  c->r[R_V0] = 0xDEADBEEFu;
  CHECK(game->hle.dispatchBios('B', 0x18));
  CHECK_EQ(c->r[R_V0], 0u);
  CHECK_EQ(game->hle.exception_exit_buf, 0u);
}

static void return_from_exception_dispatch(Core *, uint32_t resume) {
  CHECK_EQ(resume, kResume);
  dispatch_entered++;
  dispatch_game->hle.dispatchBios('B', 0x17);
  dispatch_fell_through++; // unreachable: B0:0x17 is a non-returning exception return
}

static void ordinary_return_dispatch(Core *, uint32_t resume) {
  CHECK_EQ(resume, kResume);
  dispatch_entered++;
  dispatch_fell_through++;
}

static void test_return_from_exception_unwinds_instead_of_falling_through(void) {
  const std::unique_ptr<Game> game = std::make_unique<Game>();
  write_context(&game->core, kResume);
  dispatch_entered = 0;
  dispatch_fell_through = 0;
  dispatch_game = game.get();
  game->hle.custom_exit_active = 1;

  CHECK_EQ(bios_interrupt_dispatch_custom_exit(&game->core, kBuffer, return_from_exception_dispatch),
           BiosInterruptDispatchResult::ReturnedFromException);
  game->hle.custom_exit_active = 0;
  dispatch_game = nullptr;
  CHECK_EQ(dispatch_entered, 1);
  CHECK_EQ(dispatch_fell_through, 0);
}

static void test_custom_exit_reports_an_illegal_normal_return(void) {
  const std::unique_ptr<Core> c = std::make_unique<Core>();
  write_context(c.get(), kResume);
  dispatch_entered = 0;
  dispatch_fell_through = 0;

  CHECK_EQ(bios_interrupt_dispatch_custom_exit(c.get(), kBuffer, ordinary_return_dispatch),
           BiosInterruptDispatchResult::FellThrough);
  CHECK_EQ(dispatch_entered, 1);
  CHECK_EQ(dispatch_fell_through, 1);
}

int main(void) {
  RUN(restores_the_measured_jmp_buf_layout);
  RUN(refuses_a_missing_buffer_without_clobbering);
  RUN(refuses_a_buffer_without_a_continuation);
  RUN(bios_hook_reset_and_return_entry_points);
  RUN(return_from_exception_unwinds_instead_of_falling_through);
  RUN(custom_exit_reports_an_illegal_normal_return);
  return pt_summary();
}

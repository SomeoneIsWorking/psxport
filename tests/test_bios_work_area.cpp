// test_bios_work_area — GetC0Table publishes BIOS entry addresses in ordinary guest RAM.
//
// A private HLE table previously contained only a `jr ra; nop` fallback at slots 0/1 and left every
// actual C0 entry zero. Guests obtain C(06h) ExceptionHandler through B(56h), then overwrite the
// pointed-to handler in low RAM. This drives the shipping dispatchBios boundary, preserves the old
// fallback, and proves both new answers: adjacent entries remain absent and guest writes survive a
// later GetC0Table call.
#include "testutil.h"

#include "game.h"

#include <memory>

namespace {

constexpr uint32_t kGetC0Table = 0x56u;
constexpr uint32_t kC0Table = 0x8000F800u;
constexpr uint32_t kExceptionHandlerSlot = 6u;
constexpr uint32_t kExceptionHandlerAddress = 0x00000C80u;
constexpr uint32_t kJumpReturnAddress = 0x03E00008u;

uint32_t slot_address(uint32_t slot) {
  return kC0Table + slot * sizeof(uint32_t);
}

void test_get_c0_table_publishes_exception_handler_entry_only() {
  auto game = std::make_unique<Game>();

  CHECK(game->hle.dispatchBios('B', kGetC0Table));
  CHECK_EQ(game->core.r[2], kC0Table);
  CHECK_EQ(game->core.mem_r32(slot_address(0)), kJumpReturnAddress);
  CHECK_EQ(game->core.mem_r32(slot_address(1)), 0u);
  CHECK_EQ(game->core.mem_r32(slot_address(kExceptionHandlerSlot)), kExceptionHandlerAddress);
  CHECK_EQ(game->core.mem_r32(slot_address(kExceptionHandlerSlot - 1)), 0u);
  CHECK_EQ(game->core.mem_r32(slot_address(kExceptionHandlerSlot + 1)), 0u);
}

void test_c0_table_and_exception_target_remain_guest_writable() {
  auto game = std::make_unique<Game>();
  constexpr uint32_t kGuestTableReplacement = 0x80123450u;
  constexpr uint32_t kGuestHandlerWord = 0x241A0100u;

  CHECK(game->hle.dispatchBios('B', kGetC0Table));
  game->core.mem_w32(slot_address(kExceptionHandlerSlot), kGuestTableReplacement);
  game->core.mem_w32(kExceptionHandlerAddress, kGuestHandlerWord);

  CHECK_EQ(game->core.mem_r32(slot_address(kExceptionHandlerSlot)), kGuestTableReplacement);
  CHECK_EQ(game->core.mem_r32(kExceptionHandlerAddress), kGuestHandlerWord);
  CHECK(game->hle.dispatchBios('B', kGetC0Table));
  CHECK_EQ(game->core.r[2], kC0Table);
  CHECK_EQ(game->core.mem_r32(slot_address(kExceptionHandlerSlot)), kGuestTableReplacement);
  CHECK_EQ(game->core.mem_r32(kExceptionHandlerAddress), kGuestHandlerWord);
}

} // namespace

int main() {
  RUN(get_c0_table_publishes_exception_handler_entry_only);
  RUN(c0_table_and_exception_target_remain_guest_writable);
  return pt_summary();
}

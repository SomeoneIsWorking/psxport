// Sony BIOS libc string leaves must operate on guest addresses through Core's memory map and return
// the original destination. In particular A0:0x15 is strcat: Toy Story 2 reaches it while composing
// its `.vh`/`.vb` asset paths, and leaving it unhandled aborts immediately after disc open.
#include "../runtime/recomp/game.h"
#include "testutil.h"

#include <array>
#include <cstdint>

namespace {

enum { R_V0 = 2, R_A0 = 4, R_A1 = 5 };

constexpr uint32_t kDst = 0x80100000u;
constexpr uint32_t kSrc = 0x80100100u;

void put_string(Core &core, uint32_t address, const char *text) {
  for (uint32_t i = 0;; i++) {
    core.mem_w8(address + i, static_cast<uint8_t>(text[i]));
    if (!text[i]) {
      return;
    }
  }
}

void check_string(Core &core, uint32_t address, const char *expected) {
  for (uint32_t i = 0;; i++) {
    const uint8_t want = static_cast<uint8_t>(expected[i]);
    CHECK_EQ(core.mem_r8(address + i), want);
    if (!want) {
      return;
    }
  }
}

bool strcat_call(Game &game, uint32_t destination, uint32_t source) {
  game.core.r[R_A0] = destination;
  game.core.r[R_A1] = source;
  game.core.r[R_V0] = 0xDEADBEEFu;
  return game.hle.dispatchBios('A', 0x15);
}

void check_toupper(Game &game, uint32_t character, uint32_t expected) {
  game.core.r[R_A0] = character;
  game.core.r[R_V0] = 0xDEADBEEFu;
  CHECK(game.hle.dispatchBios('A', 0x25));
  CHECK_EQ(game.core.r[R_V0], expected);
}

void test_toupper_is_ascii_only_and_locale_independent() {
  auto game = new Game();
  check_toupper(*game, static_cast<uint32_t>('t'), static_cast<uint32_t>('T'));
  check_toupper(*game, static_cast<uint32_t>('T'), static_cast<uint32_t>('T'));
  check_toupper(*game, static_cast<uint32_t>('7'), static_cast<uint32_t>('7'));
  check_toupper(*game, 0xE0u, 0xE0u);
  delete game;
}

void test_appends_terminator_returns_destination_and_preserves_guards() {
  auto game = new Game();
  put_string(game->core, kDst, "LEVEL01\\SFX");
  put_string(game->core, kSrc, ".vh");
  game->core.mem_w8(kDst - 1, 0xA5);
  game->core.mem_w8(kDst + 15, 0x5A);

  CHECK(strcat_call(*game, kDst, kSrc));
  CHECK_EQ(game->core.r[R_V0], kDst);
  check_string(game->core, kDst, "LEVEL01\\SFX.vh");
  CHECK_EQ(game->core.mem_r8(kDst - 1), 0xA5u);
  CHECK_EQ(game->core.mem_r8(kDst + 15), 0x5Au);
  delete game;
}

void test_guest_mirror_source_and_empty_inputs() {
  auto game = new Game();
  put_string(game->core, kDst, "");
  put_string(game->core, kSrc, ".vb");

  // KSEG1 0xA0100100 aliases the KSEG0 source above. Reading through Core, rather than handing host
  // pointers to libc, is part of the shipping contract.
  CHECK(strcat_call(*game, kDst, 0xA0100100u));
  check_string(game->core, kDst, ".vb");

  put_string(game->core, kSrc, "");
  CHECK(strcat_call(*game, kDst, kSrc));
  check_string(game->core, kDst, ".vb");
  delete game;
}

void test_forward_guest_alias_uses_bytewise_psx_copy_order() {
  auto game = new Game();
  constexpr std::array<uint8_t, 6> bytes = {'a', 'b', 0, 'c', 'd', 0};
  for (uint32_t i = 0; i < bytes.size(); i++) {
    game->core.mem_w8(kDst + i, bytes[i]);
  }

  // The BIOS leaf is a guest-byte loop. This alias shape terminates and distinguishes that loop from
  // calling host strcat on an unmapped guest pointer or bulk-copying a precomputed source span.
  CHECK(strcat_call(*game, kDst, kDst + 3));
  check_string(game->core, kDst, "abcd");
  delete game;
}

void test_wrong_table_and_neighbor_are_not_claimed() {
  auto game = new Game();
  put_string(game->core, kDst, "base");
  put_string(game->core, kSrc, "tail");
  game->core.r[R_V0] = 0xDEADBEEFu;

  CHECK(!game->hle.dispatchBios('Z', 0x15));
  CHECK_EQ(game->core.r[R_V0], 0xDEADBEEFu);
  CHECK(!game->hle.dispatchBios('A', 0x16));
  CHECK_EQ(game->core.r[R_V0], 0xDEADBEEFu);
  CHECK(!game->hle.dispatchBios('Z', 0x25));
  CHECK_EQ(game->core.r[R_V0], 0xDEADBEEFu);
  check_string(game->core, kDst, "base");
  delete game;
}

} // namespace

int main() {
  RUN(toupper_is_ascii_only_and_locale_independent);
  RUN(appends_terminator_returns_destination_and_preserves_guards);
  RUN(guest_mirror_source_and_empty_inputs);
  RUN(forward_guest_alias_uses_bytewise_psx_copy_order);
  RUN(wrong_table_and_neighbor_are_not_claimed);
  return pt_summary();
}

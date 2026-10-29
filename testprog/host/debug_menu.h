// debug_menu.h — the test program's debug menu: one verb per scene, and the scene as the journal's screen.
#pragma once

#include "title_debug_menu.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace psx::testprog {

inline constexpr std::array<std::string_view, 7> kSceneNames{
    "owned-square", "unowned-moving", "mmio-port", "block-dma-port", "reused-slot", "amended-square", "looping-ot"};

struct DivideCase {
  std::string_view name;
  std::uint32_t lo;
  std::uint32_t hi;
};

// The guest's divide_cases in order, with the LO and HI each leaves. Bare cases are the R3000A's; on an
// R3000A the guarded ones take break 7/6, and these are the results of the fork stripping that guard.
inline constexpr std::array<DivideCase, 7> kDivideCases{{
    {"divu-guarded 0x2D6FFC/0", 0xFFFFFFFFu, 0x002D6FFCu},
    {"div-guarded 5/0", 0xFFFFFFFFu, 5u},
    {"div-guarded -5/0", 1u, 0xFFFFFFFBu},
    {"div-guarded INT_MIN/-1", 0x80000000u, 0u},
    {"div 7/-2", 0xFFFFFFFDu, 1u},
    {"divu 7/0", 0xFFFFFFFFu, 7u},
    {"div INT_MIN/-1", 0x80000000u, 0u},
}};

class DebugMenu final : public psx::game::TitleDebugMenu {
public:
  DebugMenu();

  // The program is built from testprog/guest/testprog.c; rows cite that source, not a decompilation.
  std::optional<psx::game::DecompImage> decompImage(Core &, std::string_view, bool) const override {
    return std::nullopt;
  }
};

} // namespace psx::testprog

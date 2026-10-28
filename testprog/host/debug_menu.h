// debug_menu.h — the test program's debug menu: one verb per scene, and the scene as the journal's screen.
#pragma once

#include "title_debug_menu.h"

#include <array>
#include <string_view>

namespace psx::testprog {

inline constexpr std::array<std::string_view, 7> kSceneNames{
    "owned-square", "unowned-moving", "mmio-port", "block-dma-port", "reused-slot", "amended-square", "looping-ot"};

class DebugMenu final : public psx::game::TitleDebugMenu {
public:
  DebugMenu();

  // The program is built from testprog/guest/testprog.c; rows cite that source, not a decompilation.
  std::optional<psx::game::DecompImage> decompImage(Core &, std::string_view, bool) const override {
    return std::nullopt;
  }
};

} // namespace psx::testprog

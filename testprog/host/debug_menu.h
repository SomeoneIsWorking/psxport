// debug_menu.h — the test program's debug menu: one verb per scene, and the scene as the journal's screen.
#pragma once

#include "title_debug_menu.h"

#include <array>
#include <string_view>

namespace psx::testprog {

inline constexpr std::array<std::string_view, 5> kSceneNames{
    "owned-square", "unowned-moving", "mmio-port", "block-dma-port", "reused-slot"};

class DebugMenu final : public psx::game::TitleDebugMenu {
public:
  DebugMenu();
};

} // namespace psx::testprog

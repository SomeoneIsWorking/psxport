#include "debug_menu.h"

#include "core.h"
#include "runtime.h"

#include <lucent/log.h>

namespace psx::testprog {

using psx::game::DebugCall;
using psx::game::DebugNames;
using psx::game::DebugParam;

DebugMenu::DebugMenu() {
  screen(
      [](Core &core) {
        return std::uint64_t{core.mem_r32(session(core).facts.scene)};
      },
      [](Core &, std::uint64_t key) {
        return key < kSceneNames.size() ? lucent::format("planted {}", kSceneNames[key]) : std::string();
      });
  section("Scenes");
  verb({
      .name = "planted",
      .summary = "Show one planted scene",
      .params = {DebugParam::word("scene", DebugNames::of(std::span<const std::string_view>(kSceneNames)))},
      .run =
          [](const DebugCall &call) {
            const auto scene = static_cast<std::uint32_t>(call.values.value(0));
            call.core.mem_w32(session(call.core).facts.requestedScene, scene);
            call.reply = lucent::format("ok: scene {} from the next frame", kSceneNames[scene]);
          },
  });
}

} // namespace psx::testprog

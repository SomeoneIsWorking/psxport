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
  section("CPU");
  verb({
      .name = "divides",
      .summary = "LO and HI of the guest's divide cases against the R3000A's",
      .run =
          [](const DebugCall &call) {
            std::uint32_t at = session(call.core).facts.divideResults;
            std::size_t wrong = 0;
            std::string lines;
            for (const DivideCase &expected : kDivideCases) {
              const std::uint32_t lo = call.core.mem_r32(at);
              const std::uint32_t hi = call.core.mem_r32(at + 4u);
              at += 8u;
              const bool right = lo == expected.lo && hi == expected.hi;
              wrong += right ? 0u : 1u;
              lines +=
                  lucent::format("\n{}: lo=0x{:08X} hi=0x{:08X} {}", expected.name, lo, hi, right ? "ok" : "WRONG");
            }
            call.reply = lucent::format("{} of {} divides wrong{}", wrong, kDivideCases.size(), lines);
          },
  });
}

} // namespace psx::testprog

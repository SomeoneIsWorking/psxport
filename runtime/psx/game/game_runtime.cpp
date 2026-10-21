#include "game_runtime.h"

#include "frame_pacer.h"
#include "game.h"

namespace psx::game {

psx::cpu::PsxExeLoadResult GameRuntime::admitExecutable(Core &core, std::span<const std::uint8_t> bytes) {
  return psx::cpu::loadTopLevelPsxExe(core, bytes, "boot executable");
}

std::unique_ptr<TitleSessionState> GameRuntime::createSession(Game &) {
  return nullptr;
}

std::unique_ptr<FrameDriver> GameRuntime::createFrameDriver(Game &) {
  return nullptr;
}

std::optional<double> GameRuntime::presentDeadline(Core &core, frame::FieldCadence cadence) {
  return core.game->framePacer.subframeDeadline(core, cadence);
}

} // namespace psx::game

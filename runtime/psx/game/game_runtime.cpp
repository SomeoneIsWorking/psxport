#include "game_runtime.h"

#include "frame_pacer.h"
#include "frame_presenter.h"
#include "game.h"

namespace psx::game {

psx::cpu::PsxExeLoadResult GameRuntime::admitExecutable(Core &core, std::span<const std::uint8_t> bytes) {
  psx::cpu::PsxExeLoadResult loaded = psx::cpu::loadPsxExeImage(core, bytes, "boot executable");
  if (loaded) {
    psx::cpu::applyPsxExeTopLevelRegisters(core, loaded.image);
  }
  return loaded;
}

std::unique_ptr<TitleSessionState> GameRuntime::createSession(Game &) {
  return nullptr;
}

std::unique_ptr<FrameDriver> GameRuntime::createFrameDriver(Game &) {
  return nullptr;
}

std::unique_ptr<psx::frame::TemporalFramePresentation> GameRuntime::createTemporalFramePresentation(Game &) {
  return nullptr;
}

void GameRuntime::pacePresentation(Core &core, int guestFields, int parts) {
  core.game->framePacer.paceSubframeFields(core, guestFields, parts);
}

} // namespace psx::game

bool game_guest_vram_is_picture(const Game &game) {
  return game.runtime.guestVramIsPicture(game);
}

bool game_guest_picture_is_native_width(const Game &game) {
  return game.runtime.guestPictureIsNativeWidth(game);
}

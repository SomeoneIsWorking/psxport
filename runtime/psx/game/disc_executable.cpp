#include "disc_executable.h"

#include "game.h"
#include "title_selection.h"

namespace psx::game {

psx::cpu::PsxExeLoadResult loadDiscExecutable(Game &game, const psx::host::TitleIdentity &identity) {
  const psx::host::SelectionResult selection = psx::host::selectDiscExecutable(game.disc, identity);
  if (!selection) {
    return {.detail = selection.detail};
  }
  psx::cpu::PsxExeLoadResult loaded = psx::cpu::loadPsxExeImage(game.core, selection.bytes, identity.serial);
  if (loaded) {
    psx::cpu::applyPsxExeTopLevelRegisters(game.core, loaded.image);
  }
  return loaded;
}

} // namespace psx::game

#include "menu_readouts.h"

#include "game.h"
#include "render.h"
#include "rml_text.h" // RML_TEXT_SEP — a real U+00B7 in the DATA, never an entity
#include "title_debug_menu.h"
#include "title_debug_rows.h"

#include <lucent/log.h>

#include <cstdio>

namespace psx::ui {

MenuReadouts::MenuReadouts(Rml::Element *root, Game *game) : Component(root), mGame(game) {
  if (!mRoot) {
    return;
  }
  Rml::ElementDocument *doc = mRoot->GetOwnerDocument();
  if (!doc) {
    return;
  }
  mVideo = doc->GetElementById("video_readout");
  mStatus = doc->GetElementById("debug_status");
  mFound = (mVideo ? 1 : 0) + (mStatus ? 1 : 0);
  if (mGame) {
    const std::size_t declared = mGame->runtime.debugMenu().readouts().size();
    for (std::size_t i = 0; i < declared; ++i) {
      if (Rml::Element *element = doc->GetElementById(TitleDebugRows::readoutId(i))) {
        mTitle.emplace_back(element, i);
      }
    }
    mFound += static_cast<int>(mTitle.size());
    lucent::debug("rmlui",
                  "readouts: video={} status={} title {} of {}",
                  mVideo ? "y" : "n",
                  mStatus ? "y" : "n",
                  mTitle.size(),
                  declared);
  }
}

void MenuReadouts::update() {
  char buf[192];

  const psx::render::VideoStatus video =
      mGame != nullptr ? psx::render::videoStatus(mGame->core) : psx::render::VideoStatus{};
  // RML_TEXT_SEP is a real U+00B7 character in the TEXT, not an entity: these strings are data,
  // and `set_text` encodes them so nothing in them is ever parsed as markup (see rml_text.h).
  snprintf(buf,
           sizeof buf,
           "render %dx%d" RML_TEXT_SEP "window %dx%d" RML_TEXT_SEP "internal %dx",
           video.framebufferWidth,
           video.framebufferHeight,
           video.sinkWidth,
           video.sinkHeight,
           video.ires);
  set_text(mVideo, buf);

  if (mGame) {
    const std::span<const psx::game::DebugReadout> readouts = mGame->runtime.debugMenu().readouts();
    for (const auto &[element, index] : mTitle) {
      const psx::game::DebugReadout &readout = readouts[index];
      set_text(element, std::string(readout.label) + ": " + readout.text(mGame->core));
    }
  }

  Component::update();
}

void MenuReadouts::set_status(const std::string &text) {
  set_text(mStatus, text);
}

} // namespace psx::ui

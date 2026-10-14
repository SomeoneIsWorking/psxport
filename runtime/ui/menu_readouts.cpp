#include "menu_readouts.h"

#include "game.h"
#include "gpu_vk.h"   // gpu_vk_video_status
#include "rml_text.h" // RML_TEXT_SEP — a real U+00B7 in the DATA, never an entity

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
  mWorld = doc->GetElementById("world_readout");
  mMusic = doc->GetElementById("music_readout");
  mWarp = doc->GetElementById("warp_readout");
  mFound = (mVideo ? 1 : 0) + (mWorld ? 1 : 0) + (mMusic ? 1 : 0) + (mWarp ? 1 : 0);
  // The denominator, named. "readouts: 3 of 4" plus which one is absent is the difference between
  // a document that legitimately has no warp section and one whose id was mistyped.
  lucent::debug("rmlui",
                "readouts: {} of 4 present (video={} world={} music={} warp={})",
                mFound,
                mVideo ? "y" : "n",
                mWorld ? "y" : "n",
                mMusic ? "y" : "n",
                mWarp ? "y" : "n");
}

void MenuReadouts::update() {
  char buf[192];

  int nw = 320, ir = 1, fbw = 320, fbh = 240, ww = 0, wh = 0, cap = 4;
  gpu_vk_video_status(mGame ? &mGame->core : nullptr, &nw, &ir, &fbw, &fbh, &ww, &wh, &cap);
  // RML_TEXT_SEP is a real U+00B7 character in the TEXT, not an entity: these strings are data,
  // and `set_text` encodes them so nothing in them is ever parsed as markup (see rml_text.h).
  snprintf(
      buf, sizeof buf, "render %dx%d" RML_TEXT_SEP "window %dx%d" RML_TEXT_SEP "internal %dx", fbw, fbh, ww, wh, ir);
  set_text(mVideo, buf);

  std::string music = "stopped";
  if (const psx::game::TitleAudio *audio = mGame ? mGame->runtime.audio(mGame->core) : nullptr) {
    if (const char *name = audio->nowPlayingName()) {
      music = std::string("playing: ") + name;
    }
  }
  set_text(mMusic, music);

  set_text(mWorld, mGame ? mGame->runtime.worldReadout(mGame->core) : std::string());

  Component::update();
}

void MenuReadouts::set_warp_status(const std::string &text) {
  set_text(mWarp, text);
}

} // namespace psx::ui

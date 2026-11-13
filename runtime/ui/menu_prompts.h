// psx::ui::MenuPrompts — the document's `<glyph button="cross" key="Enter"/>` prompts drawn in the glyphs of the
// device the player last pressed (psx::input::GlyphSet).
//
// A pad shows the PSX button's glyph in its family's art (PromptFont); the keyboard shows the `key` text when the
// prompt names one, the menu's own key for that action, else the keyboard key bound to the button.
#ifndef PSXPORT_UI_MENU_PROMPTS_H
#define PSXPORT_UI_MENU_PROMPTS_H

#include "ui_component.h"

#include <cstdint>
#include <vector>

namespace psx::input {
class GlyphSet;
}

namespace psx::ui {

class MenuPrompts : public Component {
public:
  // `root` is the document body; `glyphs` may be null (the prompts then show their keyboard text).
  MenuPrompts(Rml::Element *root, const psx::input::GlyphSet *glyphs);

  // Redraws every prompt when the family changed since the last draw.
  void update() override;

private:
  const psx::input::GlyphSet *mGlyphs = nullptr;
  std::vector<Rml::Element *> mPrompts;
  std::uint32_t mDrawn = 0;
  bool mEverDrawn = false;
};

} // namespace psx::ui

#endif

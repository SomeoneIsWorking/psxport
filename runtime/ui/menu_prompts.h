// psx::ui::MenuPrompts — the document's `<glyph button="cross"/>` prompts drawn in the glyphs of the device the
// player last pressed (psx::input::GlyphSet): the bound key on the keyboard, the button in its pad family's art.
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
  // `root` is the document body; `glyphs` may be null (the prompts then stay empty).
  MenuPrompts(Rml::Element *root, const psx::input::GlyphSet *glyphs);

  // Redraws every prompt when the family changed since the last draw.
  void update() override;

private:
  struct Prompt {
    Rml::Element *element;
    std::uint16_t button;
  };
  const psx::input::GlyphSet *mGlyphs = nullptr;
  std::vector<Prompt> mPrompts;
  std::uint32_t mDrawn = 0;
  bool mEverDrawn = false;
};

} // namespace psx::ui

#endif

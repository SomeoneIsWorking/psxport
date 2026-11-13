#include "menu_prompts.h"

#include "glyph_set.h"
#include "host_input.h"
#include "rml_text.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/StringUtilities.h>
#include <lucent/log.h>

#include <string>

namespace psx::ui {

MenuPrompts::MenuPrompts(Rml::Element *root, const psx::input::GlyphSet *glyphs) : Component(root), mGlyphs(glyphs) {
  Rml::ElementList prompts;
  root->GetElementsByTagName(prompts, "glyph");
  for (Rml::Element *prompt : prompts) {
    const std::string name = rmlAttributeText(*prompt, "button", "");
    const std::uint16_t button = psx::input::buttonNamed(name);
    if (button == 0) {
      lucent::error("rmlui", "menu prompt button=\"{}\" names no PSX button; it shows nothing", name);
      continue;
    }
    mPrompts.push_back({prompt, button});
  }
}

void MenuPrompts::update() {
  if (mGlyphs == nullptr || (mEverDrawn && mGlyphs->generation() == mDrawn)) {
    return;
  }
  mEverDrawn = true;
  mDrawn = mGlyphs->generation();
  for (const Prompt &prompt : mPrompts) {
    const char32_t glyph = mGlyphs->glyph(prompt.button);
    set_text(prompt.element,
             glyph == psx::input::kNoGlyph ? std::string()
                                           : Rml::StringUtilities::ToUTF8(static_cast<Rml::Character>(glyph)));
  }
}

} // namespace psx::ui

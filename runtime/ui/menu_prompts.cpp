#include "menu_prompts.h"

#include "glyph_set.h"
#include "host_input.h"
#include "rml_text.h"

#include <RmlUi/Core/Element.h>
#include <lucent/log.h>

#include <string>

namespace psx::ui {

MenuPrompts::MenuPrompts(Rml::Element *root, const psx::input::GlyphSet *glyphs) : Component(root), mGlyphs(glyphs) {
  Rml::ElementList prompts;
  root->GetElementsByTagName(prompts, "glyph");
  for (Rml::Element *prompt : prompts) {
    const std::string name = rmlAttributeText(*prompt, "button", "");
    if (psx::input::buttonNamed(name) == 0) {
      lucent::error("rmlui", "menu prompt button=\"{}\" names no PSX button; it shows nothing", name);
      continue;
    }
    mPrompts.push_back(prompt);
  }
}

void MenuPrompts::update() {
  const std::uint32_t generation = mGlyphs != nullptr ? mGlyphs->generation() : 0;
  if (mEverDrawn && generation == mDrawn) {
    return;
  }
  mEverDrawn = true;
  mDrawn = generation;
  const bool keyboard = mGlyphs == nullptr || mGlyphs->family() == psx::input::PadFamily::Keyboard;
  for (Rml::Element *prompt : mPrompts) {
    const std::string key = rmlAttributeText(*prompt, "key", "");
    const bool asText = keyboard && !key.empty();
    prompt->SetClass("text", asText);
    const std::uint16_t button = psx::input::buttonNamed(rmlAttributeText(*prompt, "button", ""));
    set_text(prompt, asText || mGlyphs == nullptr ? key : mGlyphs->glyph(button));
  }
}

} // namespace psx::ui

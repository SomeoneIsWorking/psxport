#include "stage_band.h"

#include "menu_prompts.h"
#include "rml_text.h"

#include <RmlUi/Core/ElementDocument.h>
#include <lucent/log.h>

namespace psx::ui {

StageBand::StageBand(Rml::ElementDocument *doc, const psx::input::GlyphSet *glyphs) : Component(doc), mDoc(doc) {
  mTitles = doc->GetElementById("stage-titles");
  if (mTitles == nullptr) {
    lucent::error("rmlui", "stage.rml has no #stage-titles; the band shows no titles");
  }
  adopt<MenuPrompts>(doc, glyphs);
}

void StageBand::setTitles(std::span<const std::string> titles) {
  mNames.assign(titles.begin(), titles.end());
  draw();
}

void StageBand::select(int title) {
  if (title == mSelected) {
    return;
  }
  mSelected = title;
  draw();
}

void StageBand::draw() {
  if (mTitles == nullptr) {
    return;
  }
  std::string markup;
  for (std::size_t index = 0; index < mNames.size(); ++index) {
    markup += static_cast<int>(index) == mSelected ? "<span class=\"stage-title on\">" : "<span class=\"stage-title\">";
    markup += rmlTextMarkup(mNames[index]);
    markup += "</span>";
  }
  set_markup(mTitles, markup);
}

void StageBand::show() {
  mDoc->Show();
}

void StageBand::hide() {
  mDoc->Hide();
}

} // namespace psx::ui

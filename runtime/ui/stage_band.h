// psx::ui::StageBand — the title selector's bottom band: every title's name, the selected one bright, and
// the hint line in the glyphs of the device the player last pressed.
#ifndef PSXPORT_UI_STAGE_BAND_H
#define PSXPORT_UI_STAGE_BAND_H

#include "ui_component.h"

#include <span>
#include <string>
#include <vector>

namespace psx::input {
class GlyphSet;
}

namespace Rml {
class ElementDocument;
} // namespace Rml

namespace psx::ui {

class StageBand : public Component {
public:
  // `doc` is assets/rml/stage.rml, owned by the caller's context; `glyphs` may be null.
  StageBand(Rml::ElementDocument *doc, const psx::input::GlyphSet *glyphs);

  void setTitles(std::span<const std::string> titles);
  // The selected title's index; -1 dims every title.
  void select(int title);
  void show();
  void hide();

private:
  void draw();

  Rml::ElementDocument *mDoc = nullptr;
  Rml::Element *mTitles = nullptr;
  std::vector<std::string> mNames;
  int mSelected = -1;
};

} // namespace psx::ui

#endif

// psx::ui::ModRowModel — what a `toggle=` menu row means: the Mods field it cycles and its labels.
#ifndef PSXPORT_UI_MOD_ROW_MODEL_H
#define PSXPORT_UI_MOD_ROW_MODEL_H

#include <string>
#include <string_view>

class Mods;

namespace psx::ui {

class ModRowModel {
public:
  // Whether `id` names a toggle row, so a typo in menu.rml is reported at load.
  static bool knows(std::string_view id);

  // False removes the row: a capability this title does not have.
  static bool available(const Mods &m, std::string_view id);

  // The row's current label; false for an unknown id.
  static bool value_text(const Mods &m, std::string_view id, std::string &out);

  // Cycle the row to its next state and save the settings.
  static void toggle(Mods &m, std::string_view id);

  static int toggle_count();
};

} // namespace psx::ui

#endif

// psx::ui::ModRowModel — WHAT a menu row means, separated from the widget that draws it.
//
// The overlay's rows are two kinds: a TOGGLE cycles a named state (`Off/On`, or `Vanilla/16:9/…`)
// and an ADJUST steps a float within a range. Both used to be if-else chains repeated THREE TIMES
// in `rmlui_overlay.cpp` — once to format the value, once to toggle, once to adjust — so adding a
// row meant editing three ladders and forgetting one was silent. They are one table here, and the
// three operations are lookups into it.
//
// THE SILENT-FAILURE THIS CLOSES. The old `row_value_text` ended in `return false` for an id it did
// not recognise, and both callers treated false as "nothing to do". So `toggle="ssoa"` in menu.rml
// — a typo — produced a row that displayed its authored placeholder forever and did nothing when
// activated, with no diagnostic anywhere. Silently-skipped input is a broken instrument reporting a
// clean bill of health. `unknown_row_ids()` makes it a named, counted failure instead, reported
// once at document load with the ids it could not resolve.
#ifndef PSXPORT_UI_MOD_ROW_MODEL_H
#define PSXPORT_UI_MOD_ROW_MODEL_H

#include <string>
#include <string_view>

class Mods;

namespace psx::ui {

enum class RowKind { None, Toggle, Adjust, Action };

class ModRowModel {
public:
  // Is `id` a row this model knows about, in the given kind? Used to report unknown ids at load
  // rather than discovering them when a user presses a dead button.
  static bool knows(RowKind kind, std::string_view id);

  // Whether a known row exists for this title. Unsupported capabilities are omitted from the DOM
  // and navigation rather than left behind as inert controls.
  static bool available(const Mods &m, RowKind kind, std::string_view id);

  // Current display text for a row. Returns false (leaving `out` untouched) when the id is
  // unknown — the caller has already reported that at load time.
  static bool value_text(const Mods &m, RowKind kind, std::string_view id, std::string &out);

  // Cycle a toggle row to its next state, persisting if the row is a persisted setting.
  static void toggle(Mods &m, std::string_view id);

  // Step an adjust row by `dir` (+1/-1), clamped to the row's range, persisting.
  static void adjust(Mods &m, std::string_view id, int dir);

  // Denominators for the load-time report and for tests: how many rows of each kind exist.
  static int toggle_count();
  static int adjust_count();
};

} // namespace psx::ui

#endif

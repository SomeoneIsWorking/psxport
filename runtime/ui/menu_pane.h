// psx::ui::MenuPane — one tab's page of rows.
//
// SHAPE FROM DUSKLIGHT (CC0), `src/dusk/ui/pane.{hpp,cpp}`: the pane is the component that owns the
// rows, so "which rows exist" and "which pane is shown" are one object's business rather than two
// `GetElementsByTagName` sweeps of the whole document indexed by a bare `int`.
//
// WHAT THAT REPLACES: `setActiveTab()` used to call `GetElementsByTagName(panes, "pane")` and
// `GetElementsByTagName(tabs, "tab")` on every tab change and then index BOTH by the same integer,
// silently relying on the document authoring exactly as many `<pane>`s as `<tab>`s in the same
// order. A document with a mismatched count did not fail — it clamped with `std::min` and quietly
// dropped the tail, so a tab could exist with no page behind it and nothing said so.
//
// VISIBILITY IS `:shown`, NOT `.active`. See ui_component.h: `:active` is RmlUi's BUILT-IN
// mouse-held pseudo-class, so a pane may not use that name, and a `class` is the document author's
// namespace rather than the runtime's.
#ifndef PSXPORT_UI_MENU_PANE_H
#define PSXPORT_UI_MENU_PANE_H

#include "menu_row.h"
#include "ui_component.h"

#include <memory>

namespace psx::ui {

// Implemented by MenuDocument: it owns the models a row binds to (Mods, WarpControl, the actions),
// so the pane asks it to wire each authored row rather than reaching for those models itself.
class RowBuilder {
public:
    virtual ~RowBuilder() = default;
    // Build the binding for an authored `<select-button>`. Returning null is allowed and means "I
    // do not recognise this row"; the pane still adopts it so navigation order is unchanged, and
    // the builder is responsible for REPORTING the unknown id (a silently-skipped row is exactly
    // the failure mod_row_model.h describes).
    virtual std::unique_ptr<RowBinding> bind_row(Rml::Element* row) = 0;
    virtual void on_row_clicked(MenuRow& row) = 0;
};

class MenuPane : public Component {
public:
    MenuPane(Rml::Element* root, RowBuilder& builder);

    // Per-frame walk. A hidden pane refreshes nothing — its rows are not on screen, and refreshing
    // them was never the old behaviour either.
    void update() override;

    void set_shown(bool shown);
    bool shown() const { return mRoot && mRoot->IsPseudoClassSet("shown"); }

    // Focus this pane's first row. Returns false if it has none (the About pane, for instance).
    bool focus_first_row();

    // The row whose subtree contains `element` — used to turn RmlUi's focus element back into the
    // component that owns it. Null when the focus is not on one of this pane's rows.
    MenuRow* row_containing(Rml::Element* element) const;

    int row_count() const { return (int)mRows.size(); }
    const std::vector<MenuRow*>& rows() const { return mRows; }

private:
    // Rows are held by the Component base's mChildren (it owns their lifetime); this is the typed
    // view for the ordered operations above.
    std::vector<MenuRow*> mRows;
};

}  // namespace psx::ui

#endif

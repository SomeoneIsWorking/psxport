// psx::ui::MenuReadouts — the menu's live status lines (video / world / music / warp).
//
// A readout is a `<div id="…_readout">` the document authored; this component finds the four it
// knows, and refreshes the ones that exist. AN ABSENT READOUT IS NOT AN ERROR: a game ships its own
// `menu.rml`, and one without an Area Warp section legitimately has no `#warp_readout`. What IS
// reported, once at construction, is WHICH of them were found — so "the world line never updates"
// can be told apart from "there is no world line in this document", which the old code could not
// distinguish because every write went through a null-tolerant setter and said nothing either way.
//
// SHAPE FROM DUSKLIGHT (CC0): the readouts are their own component with their own `update()`, the
// way `src/dusk/ui/graphics_tuner.{hpp,cpp}` owns its live rows, rather than four `GetElementById`
// lookups repeated inside the overlay's per-frame function.
#ifndef PSXPORT_UI_MENU_READOUTS_H
#define PSXPORT_UI_MENU_READOUTS_H

#include "ui_component.h"

#include <cstdint>
#include <string>

class Game;

namespace psx::ui {

class MenuReadouts : public Component {
public:
    // `root` is the document body; `game` may be null (the readouts then show their static text).
    MenuReadouts(Rml::Element* root, Game* game);

    // Refresh video / world / music from live state. Called by the base's per-frame walk.
    void update() override;

    // The live world latch, pushed each frame by overlay_glue from the shown core.
    void set_world(int x, int y, int z, uint32_t stage);

    // The Area Warp status line, written when a warp is armed (or refused).
    void set_warp_status(const std::string& text);

    // Denominator for the construction report and for tests: how many of the four were present.
    int found() const { return mFound; }

private:
    Game*         mGame  = nullptr;
    Rml::Element* mVideo = nullptr;
    Rml::Element* mWorld = nullptr;
    Rml::Element* mMusic = nullptr;
    Rml::Element* mWarp  = nullptr;

    int      mWorldPos[3] = { 0, 0, 0 };
    uint32_t mWorldStage  = 0;
    bool     mWorldValid  = false;
    int      mFound       = 0;
};

}  // namespace psx::ui

#endif

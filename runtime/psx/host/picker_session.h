#pragma once

#include "picker_content.h"
#include "picker_layout.h"
#include "picker_panes.h"
#include "picker_runtime.h"
#include "title_catalog.h"

#include <memory>
#include <span>

class Game;

namespace psx::host {

class PanelArt;
struct SessionServices;

// The title selector (Slant Stage): every panel's still art side by side, presented by the host Game like a game
// frame; picking slides the seams off and names the title to start.
class PickerSession {
public:
  enum class Outcome : std::uint8_t { Chosen, Ended, DrawFault };
  struct Result {
    Outcome outcome = Outcome::Ended;
    int panel = -1; // the picked panel, set when Chosen
  };

  // `content`, `art` (one entry per panel, null for a panel with no art) and `catalog` outlive this session.
  // `selected` is the panel that stands selected first. Each selector frame is one step of the run budget; a window
  // close or a client `quit` ends the selector.
  PickerSession(PickerRuntime &runtime,
                const TitleCatalog &catalog,
                const PickerContent &content,
                std::span<const PanelArt *const> art,
                int selected,
                SessionServices &services);
  ~PickerSession();

  Result run();

private:
  // Read the pad and the control channel for one frame and act on them. True when the selected title was
  // picked.
  bool serviceInput(PickerLayout &layout);
  void select(int panel, PickerLayout &layout);

  PickerRuntime &runtime_;
  const TitleCatalog &catalog_;
  const PickerContent &content_;
  std::span<const PanelArt *const> art_;
  SessionServices &services_;
  // The Game that holds the control endpoint, reads the pad, draws the menu and presents the screen.
  std::unique_ptr<Game> host_;
  PickerPanes panes_;
  int selected_;
};

} // namespace psx::host

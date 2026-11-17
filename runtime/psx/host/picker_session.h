#pragma once

#include "picker_content.h"
#include "picker_layout.h"
#include "picker_panes.h"
#include "picker_runtime.h"
#include "stage_sides.h"
#include "title_catalog.h"

#include <memory>
#include <vector>

class Core;
class Game;

namespace psx::host {

class PanelSessions;
class TitleSession;
struct SessionServices;

// The title selector (Slant Stage): two of the stage's live titles side by side, presented by the host Game
// like a game frame; picking slides the edge out and hands the running title over.
class PickerSession {
public:
  enum class Outcome : std::uint8_t { Chosen, Ended, DrawFault };
  struct Result {
    Outcome outcome = Outcome::Ended;
    std::unique_ptr<TitleSession> session; // the picked title's session, handed over, set when Chosen
  };

  // `content` names the panels and `stage` holds their sessions; both, and `catalog`, outlive this
  // session. Each selector frame is one step of the run budget; a window close or a client `quit` ends
  // the selector.
  PickerSession(PickerRuntime &runtime,
                const TitleCatalog &catalog,
                const PickerContent &content,
                PanelSessions &stage,
                SessionServices &services);
  // Out of line: the result owns a session, complete only in the .cpp.
  ~PickerSession();

  Result run();

private:
  // Read the pad and the control channel for one frame and act on them. True when the selected title was
  // picked.
  bool serviceInput(PickerLayout &layout);
  // Apply a move of the stage's sides: the focus follows the selected side and the edge eases (or, when
  // both sides took new titles, starts again from the side the move came from).
  void applyMove(StageSides::Move move, int dir, PickerLayout &layout);

  // Every title presents into its own image at its side's size, never the window.
  void routePanes(const PickerLayout &layout);
  // The selector's present: each side's current picture built into the host's present image.
  void build(const PickerLayout &layout);
  // Attach the host to the focused panel and detach it from every other.
  void attachPaneHost(const PickerLayout &layout);

  PickerRuntime &runtime_;
  const TitleCatalog &catalog_;
  const PickerContent &content_;
  SessionServices &services_;
  // The Game that holds the control endpoint, reads the pad, draws the menu and presents the screen.
  std::unique_ptr<Game> host_;
  PickerPanes panes_;
  PanelSessions *panels_;
  std::vector<PanelSource> sources_;
  StageSides sides_;
  // A panel presented during this loop iteration, so the loop must not build or pace a second time.
  bool built_ = false;
  // The panel the host is attached to.
  int hostedPanel_ = -1;
};

} // namespace psx::host

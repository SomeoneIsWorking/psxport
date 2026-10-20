#pragma once

#include "picker_content.h"
#include "picker_layout.h"
#include "picker_panes.h"
#include "picker_runtime.h"
#include "title_catalog.h"

#include <memory>
#include <optional>
#include <vector>

class Core;
class Game;

namespace psx::host {

class PanelSessions;
class TitleSession;
struct SessionServices;

// The title selector: a product screen of live panels, presented by the host Game like a game frame.
class PickerSession {
public:
  enum class Outcome : std::uint8_t { Chosen, Ended };
  struct Result {
    Outcome outcome = Outcome::Ended;
    std::unique_ptr<TitleSession> session; // the chosen title's session, set when Chosen
  };

  // `content` names the panels; it and `catalog` outlive this session. Each selector frame is one
  // step of the run budget; a window close or a client `quit` ends the selector.
  PickerSession(PickerRuntime &runtime,
                const TitleCatalog &catalog,
                const PickerContent &content,
                SessionServices &services);
  // Out of line: the result owns a session, complete only in the .cpp.
  ~PickerSession();

  Result run();

private:
  // Read the pad and the control channel for one frame and act on them. Returns the chosen panel,
  // or -1 to keep the selector up.
  int serviceInput();

  // The selector's present: every panel's current picture built into the host's present image.
  void build();
  // Attach the host to the selected, settled panel and detach it from every other.
  void attachPaneHost();

  PickerRuntime &runtime_;
  const TitleCatalog &catalog_;
  const PickerContent &content_;
  SessionServices &services_;
  // The Game that holds the control endpoint, reads the pad, draws the menu and presents the screen.
  std::unique_ptr<Game> host_;
  std::optional<PickerLayout> layout_; // built in run() from the host Game's settings
  PickerPanes panes_;
  std::unique_ptr<PanelSessions> panels_;
  std::vector<PanelSource> sources_;
  // A panel presented during this loop iteration, so the loop must not build or pace a second time.
  bool built_ = false;
  // The panel the host is attached to, and whether it had settled then.
  int hostedPanel_ = -1;
  bool hostedSettled_ = false;
};

} // namespace psx::host

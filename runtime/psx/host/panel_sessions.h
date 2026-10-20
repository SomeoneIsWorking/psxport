// One live title session per available title, and the rules about which of them runs.
#pragma once

#include "panel_logo.h"
#include "picker_content.h"
#include "pre_roll_budget.h"
#include "render_state.h"
#include "title_availability.h"
#include "title_catalog.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

class Core;

namespace psx::host {

class TitleSession;
struct SessionServices;

class PanelSessions {
public:
  // One session per panel of `content` (the available titles `PSXPORT_PICKER_TITLES` asks for), in
  // panel order. The sessions reference the content's probed entries, which outlive the picker.
  PanelSessions(const TitleCatalog &catalog, const PickerContent &content, SessionServices &services);
  ~PanelSessions();
  PanelSessions(const PanelSessions &) = delete;
  PanelSessions &operator=(const PanelSessions &) = delete;

  int count() const {
    return static_cast<int>(sessions_.size());
  }
  // The session behind a panel, or null. Null before boot() — which is also when it has no picture.
  TitleSession *session(int panel);
  // The same handle from a const host, for a caller that only asks what a panel is showing.
  const TitleSession *session(int panel) const;
  // Whether the panel may be drawn: an unready panel is left empty, since an empty panel reads
  // as "not chosen" and a black one as "broken".
  bool panelReady(int panel) const;
  // A panel that has pre-rolled its fixed number of steps from power-on, picture or not.
  bool panelSettled(int panel) const;
  // The panel's newest picture at the size its session built it; invalid while the panel is not ready.
  psx::render::PresentedImage picture(int panel) const;
  // This panel's own logo, decoded once from its own session; a logo cannot outlive the machine
  // that decoded it.
  PanelLogo *logo(int panel);

  // Which demo the player watches and hears; the one selection owner. advance() decides which
  // session runs, since a panel with no picture must be brought up first.
  void select(int panel);
  // One panel left (-1) or right (+1), wrapping at the ends: the pad and `select left|right` alike.
  void step(int dir);
  int selected() const {
    return selected_;
  }

  // One picker frame: the selected session's step, or the next unsettled panel's pre-roll for as many
  // steps as `preRoll` allows. Returns the display fields that step waited for, so the loop paces the
  // frame itself only when the panel did not.
  unsigned long long advance(const PreRollBudget &preRoll);
  // Ends every session and hands back a fresh unbooted session for the chosen title: the demo was
  // a preview, so choosing a game must start it at power-on rather than resume its preview.
  std::unique_ptr<TitleSession> confirm();
  // What the sessions are doing, one word per panel: `panels: [0]*slug demo [1] slug booting`.
  std::string summary() const;
  // Log the summary, once per picker frame, at `lucent::debug`.
  void report() const;

private:
  // The panel to run this frame: the next one needing work, or the selected one when none do.
  int nextPanelNeedingWork();
  // Asked of the guest and not monotonic, so it only latches; see settled_.
  bool hasArrived(int panel) const;
  // Record that arrival, once.
  void latchArrival(int panel);
  // Read this panel's own logo out of that panel's own disc, once, on the first step the panel
  // runs. A no-op after the first success, and a no-op for a title whose logo has not been located.
  void takeLogo(int panel, TitleSession &session);

  const TitleCatalog &catalog_;
  const PickerContent &content_;
  std::vector<std::unique_ptr<TitleSession>> sessions_;
  // Kept because confirm() builds the fresh session the player gets.
  SessionServices &services_;
  std::vector<bool> booted_;
  // Latches on arrival: readiness is a property of the panel having arrived once, not of what its
  // guest happens to be doing this instant.
  std::vector<bool> settled_;
  std::vector<std::unique_ptr<PanelLogo>> logos_;
  int selected_ = 0;
  int running_ = -1;
  // Where the boot rotation resumes, so panels that all need work take turns.
  int mBootCursor_ = 0;
};

} // namespace psx::host

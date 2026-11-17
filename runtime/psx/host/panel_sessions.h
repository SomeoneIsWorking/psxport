// The selector's stage: one live instance per title, their lifetime across picks and returns, and which one
// the player is on.
#pragma once

#include "panel_logo.h"
#include "picker_content.h"
#include "render_state.h"
#include "title_availability.h"
#include "title_catalog.h"

#include <memory>
#include <string>
#include <vector>

class Core;

namespace psx::host {

class TitleSession;
struct SessionServices;

// Every title is loaded at once in its own machine and stays loaded. Only the focused one runs; every other
// one is paused and executes nothing once it holds its stage picture. The picked one is handed to the play loop
// and comes back on a return, never rebooted.
class PanelSessions {
public:
  // One unbooted session per panel of `content`, in panel order. The sessions reference the content's
  // probed entries, which outlive the stage.
  PanelSessions(const TitleCatalog &catalog, const PickerContent &content, SessionServices &services);
  ~PanelSessions();
  PanelSessions(const PanelSessions &) = delete;
  PanelSessions &operator=(const PanelSessions &) = delete;

  int count() const {
    return static_cast<int>(sessions_.size());
  }
  // Boot every panel not booted yet; a refused one is logged once and stays empty.
  void bootAll();
  // Run every unfocused panel, unpaced and silent, until it holds its stage picture
  // (TitleSession::holdsStagePicture), so the stage never shows an empty side.
  void preRoll();
  // The session behind a panel, or null (none, or handed over).
  TitleSession *session(int panel);
  const TitleSession *session(int panel) const;
  // Whether the panel has a picture of its own to draw.
  bool panelReady(int panel) const;
  // The panel's newest picture at the size its session built it; invalid while the panel is not ready.
  psx::render::PresentedImage picture(int panel) const;
  // The panel's icon while it has no picture: its title's own logo, read from the running title or saved from an
  // earlier read.
  PanelLogo *logo(int panel);

  // Move the focus: the old panel pauses and goes silent, the new one resumes and is heard.
  void focus(int panel);
  int focused() const {
    return focused_;
  }
  // The focused panel's machine once it has booted; null before.
  Core *focusedMachine();

  // One step of the focused session. Returns the display fields that step waited for, so the loop paces
  // the frame itself only when the panel did not.
  unsigned long long advance();
  // The focused session, running, handed over as the product (TitleSession::handOver); the others stay
  // loaded and paused, holding their pictures.
  std::unique_ptr<TitleSession> handOver();
  // The product back onto its panel, paused where it stopped. A title that ended its own run cannot go on and
  // is replaced by an unbooted one.
  void takeBack(std::unique_ptr<TitleSession> session);
  // What the sessions are doing: `panels: [0]*slug picture steps=… insns=… [1] slug cold ...`.
  std::string summary() const;
  // Log the summary at `lucent::debug`.
  void report() const;

private:
  // Read this panel's own logo from its live machine and save it once a whole frame was drawn inside its gate;
  // a no-op once read in this process.
  void takeLogo(int panel, TitleSession &session);
  // Guest instructions this panel's machine has executed; 0 before it boots.
  std::uint64_t executedInstructions(int panel) const;
  // A new paused, silent, unbooted session for this panel.
  void create(int panel);

  const TitleCatalog &catalog_;
  const PickerContent &content_;
  std::vector<std::unique_ptr<TitleSession>> sessions_;
  // Kept because takeBack() builds the sessions it replaces.
  SessionServices &services_;
  // A boot was attempted, so a refused one is not retried every frame.
  std::vector<bool> tried_;
  // Each panel's logo: the saved one until its running title gives one.
  std::vector<std::unique_ptr<PanelLogo>> logos_;
  std::vector<bool> logoRead_;
  // The previous step's read found the title's logo gate open, so this step's frame was drawn wholly inside it.
  std::vector<bool> gateOpenBefore_;
  int focused_ = 0;
};

} // namespace psx::host

// The selector's panel sessions: their lifetime and which one the player is focused on.
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

// Only the focused panel's session runs; every other one is paused and executes nothing.
class PanelSessions {
public:
  // One unbooted session per panel of `content`, in panel order. The sessions reference the content's
  // probed entries, which outlive the picker.
  PanelSessions(const TitleCatalog &catalog, const PickerContent &content, SessionServices &services);
  ~PanelSessions();
  PanelSessions(const PanelSessions &) = delete;
  PanelSessions &operator=(const PanelSessions &) = delete;

  int count() const {
    return static_cast<int>(sessions_.size());
  }
  // The session behind a panel, or null.
  TitleSession *session(int panel);
  const TitleSession *session(int panel) const;
  // Whether the panel has a picture of its own to draw.
  bool panelReady(int panel) const;
  // The panel's newest picture at the size its session built it; invalid while the panel is not ready.
  psx::render::PresentedImage picture(int panel) const;
  // The logo to draw over this panel now: its title's own logo once read, except on the focused panel
  // while its title screen, which shows that logo itself, is up.
  PanelLogo *logo(int panel);

  // Move the focus: the old panel pauses and goes silent, the new one resumes (or boots) and is heard.
  void focus(int panel);
  // One panel left (-1) or right (+1), wrapping at the ends: the pad and `select left|right` alike.
  void step(int dir);
  int focused() const {
    return focused_;
  }

  // One step of the focused session, booting it first if it never ran. Returns the display fields that
  // step waited for, so the loop paces the frame itself only when the panel did not.
  unsigned long long advance();
  // Ends every session and hands back a fresh unbooted session for the focused title: the demo was a
  // preview, so choosing a game starts it at power-on.
  std::unique_ptr<TitleSession> confirm();
  // What the sessions are doing: `panels: [0]*slug picture steps=… insns=… [1] slug cold ...`.
  std::string summary() const;
  // Log the summary at `lucent::debug`.
  void report() const;

private:
  // Read this panel's own logo from its live machine; a no-op once one is held.
  void takeLogo(int panel, TitleSession &session);
  // Guest instructions this panel's machine has executed; 0 before it boots.
  std::uint64_t executedInstructions(int panel) const;

  const TitleCatalog &catalog_;
  const PickerContent &content_;
  std::vector<std::unique_ptr<TitleSession>> sessions_;
  // Kept because confirm() builds the fresh session the player gets.
  SessionServices &services_;
  // A boot was attempted, so a refused one is not retried every frame.
  std::vector<bool> tried_;
  std::vector<std::unique_ptr<PanelLogo>> logos_;
  int focused_ = 0;
};

} // namespace psx::host

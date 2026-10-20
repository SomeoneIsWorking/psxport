#pragma once

#include "run_budget.h"
#include "title_availability.h"
#include "title_catalog.h"

#include "frame_loop_shell.h"
#include "pane_host.h"
#include "render_device.h"
#include "render_state.h"
#include "typed_config.h"

#include <cstdint>
#include <memory>

class Core;
class Game;

namespace psx::input {
class QuitScope;
}

namespace psx::host {

// What every session of the process shares, owned by ProductHost.
struct SessionServices {
  const psx::config::Config &config;
  psx::render::RenderDevice &presentation;
  psx::input::QuitScope &quit;
  RunBudget &budget;
  // Tracks which Core the process-global devices are bound to across every session.
  psx::frame::FrameLoopShell frameLoop{};
  // PSXPORT_LOAD_STATE starts the process's first product session only; a title picked after a
  // return boots from power-on.
  bool configuredStatePending = true;
};

// One boot-to-exit run of one title as a steppable owner: boot() brings the Game and guest up,
// step() advances one step, and destruction is the whole teardown. The window and presentation
// device are the product's and survive every session.
//
// Steppable rather than a self-running loop because the picker needs its titles alive at the same
// time: three sessions, one advancing and two paused, each showing its own picture. A loop that
// owns itself cannot be paused, and rebuilding a title to show its panel would restart it from
// power-on every time the player moved the selection.
//
// `services` must outlive this session.
class TitleSession {
public:
  enum class End : std::uint8_t {
    Running,            // still stepping: no end has been reached
    ReturnedToSelector, // the player (or `session return`) asked for the title selector
    TitleEnded,         // the title ended its own run; back to the selector when there is one
    BudgetSpent,        // the process's run budget ran out
    Quit,               // a window close or a client `quit`
    Refused,            // the executable could not be loaded
  };

  // A panel is a picker preview: it never counts against the run budget, takes no quit, pumps no
  // REPL, loads no configured state and owes no run-end report. The product is the confirmed or
  // directly started session, and does all of those.
  enum class Role : std::uint8_t { Panel, Product };

  TitleSession(const TitleCatalog &catalog,
               const TitleAvailability &title,
               Role role,
               bool selectorAvailable,
               SessionServices &services);
  ~TitleSession();
  TitleSession(const TitleSession &) = delete;
  TitleSession &operator=(const TitleSession &) = delete;

  // Construct the Game, boot the authenticated executable and run the init prefix. False when the
  // executable could not be loaded; a session that failed to boot has no picture and never will.
  bool boot();
  // One product step. A no-op before boot(), and the caller's business to make at most one per
  // frame.
  void step();
  // Why the session stopped; `Running` until it has.
  End end() const {
    return end_;
  }
  bool returnRequested() const;

  Core &core();
  // The title this session is running. What the host logs when the picker hands one over.
  const TitleAvailability &title() const {
    return title_;
  }
  std::uint64_t steps() const {
    return steps_;
  }
  // Display fields the last step() waited for through the field pacer. A step that paced nothing
  // (an unpaced fast-forward step, or a stub that delivers no fields) leaves the picker's frame
  // owing its wall time, because the panel did not pay it.
  unsigned long long lastStepPacedFields() const {
    return lastStepPacedFields_;
  }
  // The host compositing this session's pane, told at each pane present's build and show, so it
  // composites once per presented picture rather than once per logic frame. An empty host detaches it:
  // a session handed on as the product owns the window, and a host left attached would composite a
  // selector that is no longer on screen.
  void setPaneHost(psx::present::PaneHost host);
  // Past its retail boot prefix and drawing its own scene rather than a card the guest uploaded.
  // Both answers are the title's — its frame driver and its presentation owner — because no pixel
  // test can tell an animated publisher logo from an attract demo.
  bool showsTitlePicture() const;
  bool hasPicture() const;
  // A cold boot presents black for its first frames, and a panel that stops there is
  // indistinguishable from a title that did not start.
  //
  // The answer is about the last presented frame, so it is only asked of a session still coming
  // up: each ask is a GPU readback.
  //
  // Looking and holding are separate because looking costs a readback and holding costs nothing: a
  // paused panel must not be asked to look again. `refreshPicture()` is for a running session, or
  // one with nothing yet; `heldPicture()` is the answer, whenever it is wanted.
  bool heldPicture() const {
    return contentSeen_;
  }
  void refreshPicture() const;
  // The picture's own aspect, for the panel that will draw it. A session that has presented nothing
  // reports 1:1, which the layout treats as "the panel's own shape".
  int pictureWidth() const;
  int pictureHeight() const;
  // The newest presented frame that had a picture, so a fade or load screen between the guest's
  // scenes leaves the panel showing the last real frame.
  psx::render::PresentedImage presentedPicture() const;
  // A picture's own shape for the panel crop: its measured content rect where there is one.
  struct PaneSize {
    int width = 1, height = 1;
  };
  PaneSize pictureSize(const psx::render::PresentedImage &image) const;

  // ---- what a panel session is allowed to do
  // ------------------------------------------------------ Route this session's present into the
  // host's composite instead of the window, at `image`. See RenderState::presentToPane.
  void presentToPane(psx::present::PixelSize image);
  // Back to the ordinary product route: this session's present IS the window.
  void presentToWindow();
  // The player's controller reaches the confirmed session's guest, and the confirm press itself is
  // delivered there.
  void setPlayerInput(bool live);
  // Stop advancing / start advancing. A paused session keeps its picture and its guest state
  // exactly where they were and does not present.
  void pause();
  void resume();
  bool paused() const {
    return paused_;
  }
  // This session alone may be heard. Exactly one session in a process holds it at a time.
  void setAudible(bool audible);
  // Bringing the session up to its picture: its steps run unpaced and unheard (Game::fastForwarding).
  void setPreRolling(bool preRolling);
  // A claim does not outlive a Game, and confirming destroys the picker Game, so the session that
  // becomes the product must claim it or later commands time out on a healthy run. See
  // psxport::DbgServer::claimEndpoint.
  void claimDebugEndpoint();

private:
  struct Parts;
  // The route is held as state and applied at boot: a panel is routed before its session has
  // booted, because the host knows where every panel is long before that panel has a guest.
  void applyPresentRoute();
  // Ask the end owners once per step; true when the run ends here.
  bool endRequested(Game &game);

  const TitleCatalog &catalog_;
  const TitleAvailability &title_;
  Role role_;
  bool selectorAvailable_;
  SessionServices &services_;
  std::unique_ptr<Parts> parts_;
  End end_ = End::Running;
  bool paused_ = false;
  bool playerInput_ = false;
  bool audible_ = false;
  bool preRolling_ = false;
  bool paneDestination_ = false;
  psx::present::PixelSize paneImage_;
  // The last content answer and the step it was taken at, because the answer only changes when the
  // guest draws something new. Mutable since it is a cache of this session's own frame, not state
  // anyone else can observe.
  mutable bool contentSeen_ = false;
  mutable std::uint64_t contentCheckedAtStep_ = 0;
  std::uint64_t steps_ = 0;
  unsigned long long lastStepPacedFields_ = 0;
  // A pane host attached before boot(), waiting for a machine to attach it to.
  psx::present::PaneHost pendingPaneHost_;
};

} // namespace psx::host

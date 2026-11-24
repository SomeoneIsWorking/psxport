#pragma once

#include "run_budget.h"
#include "title_availability.h"
#include "title_catalog.h"

#include "frame_loop_shell.h"
#include "render_device.h"
#include "typed_config.h"

#include <cstdint>
#include <memory>

class Core;
class Game;

namespace psx::input {
class HostInput;
class QuitScope;
} // namespace psx::input

namespace psx::host {

// What every session of the process shares, owned by ProductHost.
struct SessionServices {
  const psx::config::Config &config;
  psx::render::RenderDevice &presentation;
  psx::input::HostInput &input;
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
    DrawFault,          // the presentation device latched a fault; nothing can be drawn again
  };

  TitleSession(const TitleCatalog &catalog,
               const TitleAvailability &title,
               bool selectorAvailable,
               SessionServices &services);
  ~TitleSession();
  TitleSession(const TitleSession &) = delete;
  TitleSession &operator=(const TitleSession &) = delete;

  // Construct the Game, boot the authenticated executable and run the init prefix. False when the
  // executable could not be loaded.
  bool boot();
  // One step. A no-op before boot().
  void step();
  // Why the session stopped; `Running` until it has.
  End end() const {
    return end_;
  }

  const TitleAvailability &title() const {
    return title_;
  }
  // A claim does not outlive a Game, and the selector's Game held the channel before this one, so the session
  // claims it or later commands time out on a healthy run. See psxport::DbgServer::claimEndpoint.
  void claimDebugEndpoint();

private:
  struct Parts;
  // Ask the end owners once per step; true when the run ends here.
  bool endRequested(Game &game);

  const TitleCatalog &catalog_;
  const TitleAvailability &title_;
  bool selectorAvailable_;
  SessionServices &services_;
  std::unique_ptr<Parts> parts_;
  End end_ = End::Running;
  std::uint64_t steps_ = 0;
};

} // namespace psx::host

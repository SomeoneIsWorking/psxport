#include "panel_sessions.h"

#include "panel_logo.h"
#include "title_availability.h"
#include "title_session.h"

#include "core.h"
#include "game_runtime.h"
#include "gpu_vk.h"

#include <lucent/log.h>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>

namespace psx::host {
namespace {

// How long a panel pre-rolls from power-on before it holds: past the publisher cards onto
// the title's own press-start page or opening movie.
constexpr int kPreRollSteps = 900;

} // namespace

PanelSessions::PanelSessions(const TitleCatalog &catalog, const PickerContent &content, GpuDevice &presentation)
    : catalog_(catalog), presentation_(presentation) {
  for (int panel = 0; panel < content.panelCount(); ++panel) {
    titles_.push_back(&content.panelTitle(panel));
  }
  sessions_.reserve(titles_.size());
  booted_.assign(titles_.size(), false);
  settled_.assign(titles_.size(), false);
  logos_.resize(titles_.size());
  for (const TitleAvailability *title : titles_) {
    // Constructed, not booted: an unasked session costs only its handle, and boot() is the
    // expensive part.
    sessions_.push_back(std::make_unique<TitleSession>(catalog_, *title, true, presentation));
  }
  if (sessions_.empty()) {
    lucent::warn("picker", "no provisioned title: the selector has no panels");
    return;
  }
  // The first panel is the one the player is on, and it is the one that boots first.
  select(0);
  lucent::info("picker", "{} title panel(s) over the probed catalog", count());
}

PanelSessions::~PanelSessions() = default;

const TitleAvailability &PanelSessions::title(int panel) const {
  if (panel < 0 || panel >= count()) {
    lucent::error("picker", "panel {} is outside the {} panel(s) that exist", panel, count());
    std::abort();
  }
  return *titles_[static_cast<std::size_t>(panel)];
}

int PanelSessions::panelOf(std::string_view slug) const {
  for (int panel = 0; panel < count(); ++panel) {
    if (titles_[static_cast<std::size_t>(panel)]->identity->slug == slug) {
      return panel;
    }
  }
  return -1;
}

TitleSession *PanelSessions::session(int panel) {
  if (panel < 0 || panel >= count()) {
    return nullptr;
  }
  return sessions_[static_cast<std::size_t>(panel)].get();
}

const TitleSession *PanelSessions::session(int panel) const {
  if (panel < 0 || panel >= count()) {
    return nullptr;
  }
  return sessions_[static_cast<std::size_t>(panel)].get();
}

bool PanelSessions::hasPicture(int panel) const {
  if (panel < 0 || panel >= count() || !booted_[static_cast<std::size_t>(panel)]) {
    return false;
  }
  return sessions_[static_cast<std::size_t>(panel)]->hasPicture();
}

// Past the title's boot prefix and holding a picture: a publisher logo passes the picture test
// and fails the prefix.
bool PanelSessions::panelReady(int panel) const {
  if (panel < 0 || panel >= count() || !booted_[static_cast<std::size_t>(panel)]) {
    return false;
  }
  const TitleSession *session = this->session(panel);
  // Both, in this order: past the boot prefix (its own driver says so) and holding a frame
  // (the probe says so).
  if (session == nullptr || !session->showsTitlePicture()) {
    return false;
  }
  // A panel holding a picture is not probed again: its picture is whatever it last presented.
  // This is the only place a session is probed outside the one that is running.
  if (!session->heldPicture()) {
    session->refreshPicture();
  }
  return session->heldPicture();
}

bool PanelSessions::hasVisiblePicture(int panel) const {
  return panelReady(panel);
}

int PanelSessions::pictureWidth(int panel) const {
  if (panel < 0 || panel >= count() || !booted_[static_cast<std::size_t>(panel)]) {
    return 1;
  }
  return sessions_[static_cast<std::size_t>(panel)]->pictureWidth();
}

int PanelSessions::pictureHeight(int panel) const {
  if (panel < 0 || panel >= count() || !booted_[static_cast<std::size_t>(panel)]) {
    return 1;
  }
  return sessions_[static_cast<std::size_t>(panel)]->pictureHeight();
}

Core *PanelSessions::core(int panel) {
  if (!hasVisiblePicture(panel)) {
    return nullptr;
  }
  return &sessions_[static_cast<std::size_t>(panel)]->core();
}

PanelLogo *PanelSessions::logo(int panel) {
  if (panel < 0 || panel >= count()) {
    return nullptr;
  }
  const std::unique_ptr<PanelLogo> &logo = logos_[static_cast<std::size_t>(panel)];
  return logo != nullptr ? logo.get() : nullptr;
}

void PanelSessions::takeLogo(int panel, TitleSession &session) {
  const std::size_t index = static_cast<std::size_t>(panel);
  if (index >= logos_.size() || logos_[index] != nullptr) {
    return; // already have it, or there is no such panel
  }
  if (session.steps() == 0) {
    return; // the panel has not run yet, so its disc has not been opened
  }
  Core &core = session.core();
  // The first successful decode is the logo and is kept for the life of the panel.
  if (std::optional<LogoImage> image = core.runtime->panelLogo(core)) {
    logos_[index] = std::make_unique<PanelLogo>(image->width, image->height, std::move(image->rgba));
  }
}

void PanelSessions::select(int panel) {
  if (panel < 0 || panel >= count() || panel == selected_) {
    return;
  }
  selected_ = panel;
  // The demo the player is looking at is the one they hear. The host audio device is a per-process
  // resource, so every other session must release it.
  for (int other = 0; other < count(); ++other) {
    if (TitleSession *session = this->session(other)) {
      session->setAudible(other == panel);
    }
  }
  lucent::info("picker", "selected panel {}: {}", panel, title(panel).identity->displayName);
}

bool PanelSessions::panelSettled(int panel) const {
  return panel >= 0 && panel < count() && settled_[static_cast<std::size_t>(panel)];
}

bool PanelSessions::hasArrived(int panel) const {
  if (!panelReady(panel)) {
    return false;
  }
  const TitleSession *session = this->session(panel);
  if (session == nullptr) {
    return false;
  }
  return session->steps() >= kPreRollSteps;
}

void PanelSessions::latchArrival(int panel) {
  if (panel < 0 || panel >= count() || settled_[static_cast<std::size_t>(panel)]) {
    return;
  }
  if (hasArrived(panel)) {
    settled_[static_cast<std::size_t>(panel)] = true;
    lucent::info("picker",
                 "panel {} ({}) finished its pre-roll and now runs as the product",
                 panel,
                 title(panel).identity->displayName);
  }
}

int PanelSessions::nextPanelNeedingWork() {
  // The selected panel is the product: it runs at the player's own speed. Every other panel is a
  // paused still picture, which costs nothing, except one that has never reached its own picture,
  // since its title's opening cards leave only by running.
  //
  // Pre-rolls rotate one slice each rather than one panel all the way, so the selector takes about
  // as long as the slowest boot instead of the sum. Still one guest at a time: this picks which
  // session gets this frame, it never runs two. A settled panel is chosen again only once the
  // selection moves onto it, and resumes from the frame it froze on.
  for (int turn = 0; turn < count(); ++turn) {
    const int panel = (mBootCursor_ + turn) % count();
    if (!panelSettled(panel)) {
      mBootCursor_ = (panel + 1) % count();
      return panel;
    }
  }
  // Everything is settled: the product runs, and it runs alone.
  return selected_;
}

bool PanelSessions::advance(int bootStepBudget) {
  const int panel = nextPanelNeedingWork();
  if (panel < 0 || count() == 0) {
    return false;
  }
  TitleSession *current = session(panel);
  if (current == nullptr) {
    return false;
  }
  // Switching is pause-then-resume, never two at once: the frame a panel gets to bring itself up
  // is the frame the selected panel does not run.
  if (running_ != panel) {
    if (TitleSession *previous = session(running_)) {
      previous->pause();
    }
    current->resume();
    running_ = panel;
  }
  if (!booted_[static_cast<std::size_t>(panel)]) {
    if (!current->boot()) {
      lucent::error("picker", "{} could not be booted: its panel stays empty", title(panel).identity->displayName);
      booted_[static_cast<std::size_t>(panel)] = true;
      return false;
    }
    booted_[static_cast<std::size_t>(panel)] = true;
    lucent::info("picker", "booting panel {}: {}", panel, title(panel).identity->displayName);
  }
  if (current->returnRequested()) {
    return false;
  }
  // An arrived panel gets one step per picker frame: it is the product the player is watching, and
  // the picker's cadence is one display field. A pre-rolling panel gets the whole budget, since
  // it is booting rather than playing.
  const bool preRolling = !panelSettled(panel);
  const int budget = preRolling ? std::max(bootStepBudget, 1) : 1;
  bool advanced = false;
  for (int step = 0; step < budget; ++step) {
    if (current->end() != TitleSession::End::Running) {
      break;
    }
    current->step();
    advanced = true;
    // The running panel is the only one whose picture can have changed, so it is the only one worth
    // reading back; its own slow cadence is what keeps a demo playing in the panel.
    current->refreshPicture();
    // Once, from this session's VRAM while its own screen is still up.
    takeLogo(panel, *current);
    if (current->end() != TitleSession::End::Running) {
      lucent::info("picker",
                   "panel {} ({}) ended its run; its panel keeps the picture it last presented",
                   panel,
                   title(panel).identity->displayName);
      break;
    }
    // Asked of every unsettled panel each frame, not only the running one: a panel the player is
    // not looking at must stop consuming the boot budget the moment it reaches its demo. Asking
    // costs a question, not a step.
    for (int other = 0; other < count(); ++other) {
      latchArrival(other);
    }
  }
  return advanced;
}

std::unique_ptr<TitleSession> PanelSessions::confirm() {
  if (count() == 0) {
    return nullptr;
  }
  const int chosen = selected_;
  const TitleAvailability &picked = title(chosen);
  // The chosen panel's demo is a preview, so it dies with the rest: choosing a game means booting
  // it, not resuming someone's attract loop with its memory card writes.
  //
  // The fresh session boots on the product's own terms with the window and its device, which
  // belong to the process rather than to any session.
  sessions_.clear();
  booted_.assign(titles_.size(), false);
  // An arrival belongs to the machine that measured it, so coming back to the selector re-asks
  // every title.
  settled_.assign(titles_.size(), false);
  running_ = -1;
  // Panel defaults are undone before boot, and the player's pad must reach the guest: a panel
  // session starts with player input suppressed so the press that moved the selection does not
  // also press a button in three demos at once.
  auto session = std::make_unique<TitleSession>(catalog_, picked, true, presentation_);
  session->presentToWindow();
  session->setPlayerInput(true);
  session->setAudible(true);
  lucent::info("picker",
               "confirmed panel {}: {} boots fresh into the window (its preview demo is discarded)",
               chosen,
               picked.identity->displayName);
  return session;
}

void PanelSessions::report() const {
  std::string line = "panels:";
  for (int panel = 0; panel < count(); ++panel) {
    const bool isSelected = panel == selected_;
    line += std::format(" [{}]{}{} {}",
                        panel,
                        isSelected ? "*" : " ",
                        titles_[static_cast<std::size_t>(panel)]->identity->slug,
                        panelReady(panel) ? (panelSettled(panel) ? "demo" : "picture")
                                          : (booted_[static_cast<std::size_t>(panel)] ? "booting" : "cold"));
  }
  lucent::debug("picker", "{}", line);
}

} // namespace psx::host

#include "panel_sessions.h"

#include "panel_logo.h"
#include "title_availability.h"
#include "title_session.h"

#include "core.h"
#include "game.h"
#include "game_runtime.h"
#include "wrap.h"

#include <lucent/log.h>

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

PanelSessions::PanelSessions(const TitleCatalog &catalog, const PickerContent &content, SessionServices &services)
    : catalog_(catalog), content_(content), services_(services) {
  const auto panels = static_cast<std::size_t>(content_.panelCount());
  sessions_.reserve(panels);
  booted_.assign(panels, false);
  settled_.assign(panels, false);
  logos_.resize(panels);
  for (int panel = 0; panel < content_.panelCount(); ++panel) {
    // Constructed, not booted: boot() is the expensive part.
    sessions_.push_back(std::make_unique<TitleSession>(
        catalog_, content_.panelTitle(panel), TitleSession::Role::Panel, true, services_));
  }
  if (sessions_.empty()) {
    lucent::warn("picker", "no provisioned title: the selector has no panels");
    return;
  }
  // The first panel is the one the player is on: it boots first and is the one heard. select(0)
  // would be a no-op, since panel 0 is already the selection.
  sessions_.front()->setAudible(true);
  lucent::info("picker", "{} title panel(s) over the probed catalog", count());
}

PanelSessions::~PanelSessions() = default;

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

bool PanelSessions::panelReady(int panel) const {
  if (panel < 0 || panel >= count() || !booted_[static_cast<std::size_t>(panel)]) {
    return false;
  }
  const TitleSession *session = this->session(panel);
  return session != nullptr && session->showsTitlePicture();
}

psx::render::PresentedImage PanelSessions::picture(int panel) const {
  if (!panelReady(panel)) {
    return {};
  }
  return sessions_[static_cast<std::size_t>(panel)]->presentedPicture();
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
  if (std::optional<LogoImage> image = core.runtime.panelLogo(core)) {
    logos_[index] = std::make_unique<PanelLogo>(std::move(*image));
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
  lucent::info("picker", "selected panel {}: {}", panel, content_.panelTitle(panel).identity->displayName);
}

void PanelSessions::step(int dir) {
  select(psx::ui::wrap(selected_, dir, count()));
}

bool PanelSessions::panelSettled(int panel) const {
  return panel >= 0 && panel < count() && settled_[static_cast<std::size_t>(panel)];
}

// The pre-roll is a step count, not a picture: a panel that never draws one must still stop
// taking the frames the selected panel runs in.
bool PanelSessions::hasArrived(int panel) const {
  if (panel < 0 || panel >= count() || !booted_[static_cast<std::size_t>(panel)]) {
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
                 content_.panelTitle(panel).identity->displayName);
  }
}

int PanelSessions::nextPanelNeedingWork() {
  // The panel the player is on takes every slice until it shows a picture.
  if (!panelSettled(selected_) && !panelReady(selected_)) {
    return selected_;
  }
  // Unsettled panels pre-roll in turn, one slice each, so the wait is the slowest boot, not the sum.
  for (int turn = 0; turn < count(); ++turn) {
    const int panel = (mBootCursor_ + turn) % count();
    if (!panelSettled(panel)) {
      mBootCursor_ = (panel + 1) % count();
      return panel;
    }
  }
  return selected_;
}

unsigned long long PanelSessions::advance(const PreRollBudget &preRoll) {
  const int panel = nextPanelNeedingWork();
  if (panel < 0 || count() == 0) {
    return 0;
  }
  TitleSession *current = session(panel);
  if (current == nullptr) {
    return 0;
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
      lucent::error(
          "picker", "{} could not be booted: its panel stays empty", content_.panelTitle(panel).identity->displayName);
      booted_[static_cast<std::size_t>(panel)] = true;
      return 0;
    }
    booted_[static_cast<std::size_t>(panel)] = true;
    lucent::info("picker", "booting panel {}: {}", panel, content_.panelTitle(panel).identity->displayName);
  }
  if (current->returnRequested()) {
    return 0;
  }
  // An arrived panel gets one step per picker frame on its own pacer, and the fields that step
  // waited for are the frame's wall time: an unpaced fast-forward step pays nothing and owes the
  // loop's field pace, which is what the return value tells the loop.
  current->setPreRolling(!panelSettled(panel));
  // The selector's menu writes the settings; the panel that runs shows them.
  current->core().game->mods.reload();
  unsigned long long pacedFields = 0;
  PreRollBudget::Clock::duration lastStep{};
  for (int step = 0; current->end() == TitleSession::End::Running; ++step) {
    if (step > 0 && (panelSettled(panel) || !preRoll.mayStep(step, PreRollBudget::Clock::now(), lastStep))) {
      break;
    }
    const PreRollBudget::Clock::time_point stepStart = PreRollBudget::Clock::now();
    current->step();
    lastStep = PreRollBudget::Clock::now() - stepStart;
    pacedFields += current->lastStepPacedFields();
    // Once, from this session's VRAM while its own screen is still up.
    takeLogo(panel, *current);
    if (current->end() != TitleSession::End::Running) {
      lucent::info("picker",
                   "panel {} ({}) ended its run; its panel keeps the picture it last presented",
                   panel,
                   content_.panelTitle(panel).identity->displayName);
      break;
    }
    // Every panel, so one that reached its demo stops taking pre-roll turns.
    for (int other = 0; other < count(); ++other) {
      latchArrival(other);
    }
  }
  return pacedFields;
}

std::unique_ptr<TitleSession> PanelSessions::confirm() {
  if (count() == 0) {
    return nullptr;
  }
  const int chosen = selected_;
  const TitleAvailability &picked = content_.panelTitle(chosen);
  // The chosen title boots fresh; its preview demo is discarded with the rest.
  sessions_.clear();
  booted_.assign(booted_.size(), false);
  settled_.assign(settled_.size(), false);
  running_ = -1;
  auto session = std::make_unique<TitleSession>(catalog_, picked, TitleSession::Role::Product, true, services_);
  session->presentToWindow();
  session->setPlayerInput(true);
  session->setAudible(true);
  lucent::info("picker",
               "confirmed panel {}: {} boots fresh into the window (its preview demo is discarded)",
               chosen,
               picked.identity->displayName);
  return session;
}

std::string PanelSessions::summary() const {
  std::string line = "panels:";
  for (int panel = 0; panel < count(); ++panel) {
    const bool isSelected = panel == selected_;
    line += std::format(" [{}]{}{} {}",
                        panel,
                        isSelected ? "*" : " ",
                        content_.panelTitle(panel).identity->slug,
                        panelReady(panel) ? (panelSettled(panel) ? "demo" : "picture")
                                          : (booted_[static_cast<std::size_t>(panel)] ? "booting" : "cold"));
  }
  return line;
}

void PanelSessions::report() const {
  lucent::debug("picker", "{}", summary());
}

} // namespace psx::host

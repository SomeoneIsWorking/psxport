#include "panel_sessions.h"

#include "panel_logo.h"
#include "title_availability.h"
#include "title_session.h"

#include "core.h"
#include "game.h"
#include "game_runtime.h"
#include "guest_executor.h"

#include <lucent/log.h>

#include <format>
#include <optional>
#include <string>
#include <utility>

namespace psx::host {

PanelSessions::PanelSessions(const TitleCatalog &catalog, const PickerContent &content, SessionServices &services)
    : catalog_(catalog), content_(content), services_(services) {
  const auto panels = static_cast<std::size_t>(content_.panelCount());
  sessions_.resize(panels);
  tried_.assign(panels, false);
  logos_.resize(panels);
  logoRead_.assign(panels, false);
  gateOpenBefore_.assign(panels, false);
  for (int panel = 0; panel < content_.panelCount(); ++panel) {
    // A panel shows its logo from the first frame without running its title.
    if (std::optional<LogoImage> saved = SavedLogo(*content_.panelTitle(panel).identity).load()) {
      logos_[static_cast<std::size_t>(panel)] = std::make_unique<PanelLogo>(std::move(*saved));
    }
    create(panel);
  }
  if (sessions_.empty()) {
    lucent::warn("picker", "no provisioned title: the selector has no panels");
    return;
  }
  sessions_.front()->resume();
  sessions_.front()->setAudible(true);
  lucent::info("picker", "{} title panel(s) over the probed catalog", count());
}

PanelSessions::~PanelSessions() = default;

void PanelSessions::create(int panel) {
  const auto index = static_cast<std::size_t>(panel);
  auto session = std::make_unique<TitleSession>(
      catalog_, content_.panelTitle(panel), TitleSession::Role::Panel, true, services_);
  session->pause();
  sessions_[index] = std::move(session);
  tried_[index] = false;
  logoRead_[index] = false;
  gateOpenBefore_[index] = false;
}

void PanelSessions::bootAll() {
  for (int panel = 0; panel < count(); ++panel) {
    const auto index = static_cast<std::size_t>(panel);
    TitleSession *session = sessions_[index].get();
    if (session == nullptr || tried_[index]) {
      continue;
    }
    tried_[index] = true;
    if (!session->boot()) {
      lucent::error(
          "picker", "{} could not be booted: its panel stays empty", content_.panelTitle(panel).identity->displayName);
      continue;
    }
    lucent::info("picker", "booted panel {}: {}", panel, content_.panelTitle(panel).identity->displayName);
  }
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

bool PanelSessions::panelReady(int panel) const {
  const TitleSession *held = session(panel);
  return held != nullptr && held->showsTitlePicture();
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
  PanelLogo *held = logos_[static_cast<std::size_t>(panel)].get();
  if (held == nullptr) {
    return nullptr;
  }
  TitleSession *session = sessions_[static_cast<std::size_t>(panel)].get();
  if (session == nullptr || !session->booted()) {
    return held;
  }
  // A paused machine still holds the state its last picture was drawn from.
  Core &core = session->core();
  return core.runtime.showsOwnLogo(core) ? nullptr : held;
}

void PanelSessions::takeLogo(int panel, TitleSession &session) {
  const auto index = static_cast<std::size_t>(panel);
  if (logoRead_[index]) {
    return;
  }
  Core &core = session.core();
  std::optional<LogoImage> image = core.runtime.panelLogo(core);
  // The gate is read after the step, so the frame of the step it first opens on may predate it.
  const bool drawnInsideGate = image && gateOpenBefore_[index];
  gateOpenBefore_[index] = image.has_value();
  if (drawnInsideGate) {
    lucent::info("picker",
                 "{}: its own logo read from its title screen, {}x{}",
                 session.title().identity->displayName,
                 image->width,
                 image->height);
    logoRead_[index] = true;
    SavedLogo(*session.title().identity).save(*image);
    logos_[index] = std::make_unique<PanelLogo>(std::move(*image));
  }
}

void PanelSessions::focus(int panel) {
  if (panel < 0 || panel >= count() || panel == focused_) {
    return;
  }
  if (TitleSession *previous = session(focused_)) {
    previous->pause();
    previous->setAudible(false);
  }
  focused_ = panel;
  if (TitleSession *next = session(focused_)) {
    next->setAudible(true);
    next->resume();
  }
  lucent::info("picker", "on {}", content_.panelTitle(panel).identity->displayName);
}

Core *PanelSessions::focusedMachine() {
  TitleSession *current = session(focused_);
  return current != nullptr && current->booted() ? &current->core() : nullptr;
}

unsigned long long PanelSessions::advance() {
  TitleSession *held = session(focused_);
  if (held == nullptr) {
    return 0;
  }
  TitleSession &current = *held;
  if (!current.booted() || current.end() != TitleSession::End::Running || current.returnRequested()) {
    return 0;
  }
  // The selector's menu writes the settings; the panel that runs shows them.
  current.core().game->mods.reload();
  current.step();
  takeLogo(focused_, current);
  if (current.end() != TitleSession::End::Running) {
    lucent::info("picker",
                 "panel {} ({}) ended its run; its panel keeps the picture it last presented",
                 focused_,
                 content_.panelTitle(focused_).identity->displayName);
  }
  return current.lastStepPacedFields();
}

std::unique_ptr<TitleSession> PanelSessions::handOver() {
  if (session(focused_) == nullptr) {
    return nullptr;
  }
  for (int panel = 0; panel < count(); ++panel) {
    if (panel != focused_) {
      sessions_[static_cast<std::size_t>(panel)].reset();
    }
  }
  std::unique_ptr<TitleSession> picked = std::move(sessions_[static_cast<std::size_t>(focused_)]);
  lucent::info("picker", "picked {}", content_.panelTitle(focused_).identity->displayName);
  picked->handOver();
  return picked;
}

void PanelSessions::takeBack(std::unique_ptr<TitleSession> session) {
  const auto index = static_cast<std::size_t>(focused_);
  if (session != nullptr && session->end() != TitleSession::End::TitleEnded) {
    session->takeBack();
    sessions_[index] = std::move(session);
  } else {
    session.reset();
    create(focused_);
  }
  for (int panel = 0; panel < count(); ++panel) {
    if (sessions_[static_cast<std::size_t>(panel)] == nullptr) {
      create(panel);
    }
  }
  sessions_[index]->resume();
  sessions_[index]->setAudible(true);
}

std::uint64_t PanelSessions::executedInstructions(int panel) const {
  // The pointer, not the const accessor: reading a counter asks the session for its Core.
  TitleSession *held = sessions_[static_cast<std::size_t>(panel)].get();
  if (held == nullptr || !held->booted()) {
    return 0;
  }
  const psx::cpu::ExecutorCounters &counters = held->core().executor().counters();
  return counters.executedInstructions;
}

std::string PanelSessions::summary() const {
  std::string line = "panels:";
  for (int panel = 0; panel < count(); ++panel) {
    const TitleSession *held = session(panel);
    line += std::format(" [{}]{}{} {} steps={} insns={}",
                        panel,
                        panel == focused_ ? "*" : " ",
                        content_.panelTitle(panel).identity->slug,
                        held == nullptr ? "handed-over"
                                        : (panelReady(panel) ? "picture" : (held->booted() ? "booting" : "cold")),
                        held == nullptr ? 0 : held->steps(),
                        executedInstructions(panel));
  }
  return line;
}

void PanelSessions::report() const {
  lucent::debug("picker", "{}", summary());
}

} // namespace psx::host

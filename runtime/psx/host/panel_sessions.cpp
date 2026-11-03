#include "panel_sessions.h"

#include "panel_logo.h"
#include "title_availability.h"
#include "title_session.h"

#include "core.h"
#include "game.h"
#include "game_runtime.h"
#include "lightrec_executor.h"
#include "wrap.h"

#include <lucent/log.h>

#include <format>
#include <optional>
#include <string>
#include <utility>

namespace psx::host {

PanelSessions::PanelSessions(const TitleCatalog &catalog, const PickerContent &content, SessionServices &services)
    : catalog_(catalog), content_(content), services_(services) {
  const auto panels = static_cast<std::size_t>(content_.panelCount());
  sessions_.reserve(panels);
  tried_.assign(panels, false);
  logos_.resize(panels);
  logoRead_.assign(panels, false);
  gateOpenBefore_.assign(panels, false);
  for (int panel = 0; panel < content_.panelCount(); ++panel) {
    // A panel shows its logo from the first frame without running its title.
    if (std::optional<LogoImage> saved = SavedLogo(*content_.panelTitle(panel).identity).load()) {
      logos_[static_cast<std::size_t>(panel)] = std::make_unique<PanelLogo>(std::move(*saved));
    }
    auto session = std::make_unique<TitleSession>(
        catalog_, content_.panelTitle(panel), TitleSession::Role::Panel, true, services_);
    session->pause();
    sessions_.push_back(std::move(session));
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
  if (panel < 0 || panel >= count()) {
    return false;
  }
  return sessions_[static_cast<std::size_t>(panel)]->showsTitlePicture();
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
  TitleSession &session = *sessions_[static_cast<std::size_t>(panel)];
  if (!session.booted()) {
    return held;
  }
  // A paused machine still holds the state its last picture was drawn from.
  Core &core = session.core();
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
  TitleSession &previous = *sessions_[static_cast<std::size_t>(focused_)];
  previous.pause();
  previous.setAudible(false);
  focused_ = panel;
  TitleSession &next = *sessions_[static_cast<std::size_t>(focused_)];
  next.setAudible(true);
  next.resume();
  lucent::info("picker", "focused panel {}: {}", panel, content_.panelTitle(panel).identity->displayName);
}

Core *PanelSessions::focusedMachine() {
  if (count() == 0) {
    return nullptr;
  }
  TitleSession &current = *sessions_[static_cast<std::size_t>(focused_)];
  return current.booted() ? &current.core() : nullptr;
}

void PanelSessions::step(int dir) {
  focus(psx::ui::wrap(focused_, dir, count()));
}

unsigned long long PanelSessions::advance() {
  if (count() == 0) {
    return 0;
  }
  const std::size_t index = static_cast<std::size_t>(focused_);
  TitleSession &current = *sessions_[index];
  if (!tried_[index]) {
    tried_[index] = true;
    if (!current.boot()) {
      lucent::error("picker",
                    "{} could not be booted: its panel stays empty",
                    content_.panelTitle(focused_).identity->displayName);
      return 0;
    }
    lucent::info("picker", "booted panel {}: {}", focused_, content_.panelTitle(focused_).identity->displayName);
  }
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

std::unique_ptr<TitleSession> PanelSessions::confirm() {
  if (count() == 0) {
    return nullptr;
  }
  const int chosen = focused_;
  const TitleAvailability &picked = content_.panelTitle(chosen);
  sessions_.clear();
  tried_.assign(tried_.size(), false);
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

std::uint64_t PanelSessions::executedInstructions(int panel) const {
  TitleSession &session = *sessions_[static_cast<std::size_t>(panel)];
  if (!session.booted()) {
    return 0;
  }
  const psx::cpu::ExecutorCounters &counters = session.core().lightrecExecutor().counters();
  return counters.executedInstructions + counters.fallback.instructions;
}

std::string PanelSessions::summary() const {
  std::string line = "panels:";
  for (int panel = 0; panel < count(); ++panel) {
    const TitleSession &session = *sessions_[static_cast<std::size_t>(panel)];
    line += std::format(" [{}]{}{} {} steps={} insns={}",
                        panel,
                        panel == focused_ ? "*" : " ",
                        content_.panelTitle(panel).identity->slug,
                        panelReady(panel) ? "picture" : (session.booted() ? "booting" : "cold"),
                        session.steps(),
                        executedInstructions(panel));
  }
  return line;
}

void PanelSessions::report() const {
  lucent::debug("picker", "{}", summary());
}

} // namespace psx::host

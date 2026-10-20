#include "snapshot.h"

#include "core.h"
#include "diagnostic_config.h"
#include "fs_util.h"

#include <lucent/log.h>

#include <csignal>
#include <string>
#include <unistd.h>

namespace {

// Only a sig_atomic_t is safe to touch from the handler.
volatile sig_atomic_t g_sigRequested = 0;

void onUsr1(int) {
  g_sigRequested = 1;
}

} // namespace

namespace psx::debug {

bool Snapshotter::writeRam(const Core &core, const std::string &path) {
  const std::string spad = path + ".spad";
  std::string error;
  if (!Fs::writeFile(path, core.ram, sizeof core.ram, error) ||
      !Fs::writeFile(spad, core.scratch, sizeof core.scratch, error)) {
    lucent::error("snap", "{}", error);
    return false;
  }
  lucent::info("snap", "guest RAM -> {} (+ .spad)", path);
  return true;
}

void Snapshotter::configure() {
  configured_ = true;
  const psx::config::SnapshotSchedule schedule = psx::config::snapshot_schedule();
  at_ = FrameSchedule(schedule.at);
  every_ = schedule.every;
  max_ = schedule.max;
  signal(SIGUSR1, onUsr1);
  lucent::debug("snap", "scheduled {} frame(s), every={}, max={} (kill -USR1 {})", at_.size(), every_, max_, getpid());
}

void Snapshotter::capture(const Core &core, std::uint32_t frame, const char *why) {
  if (written_ >= max_) {
    lucent::warn("snap", "PSXPORT_SNAP_MAX={} reached; frame {} ({}) not written", max_, frame, why);
    return;
  }
  if (writeRam(core, "scratch/raw/snap_" + std::to_string(frame) + ".bin")) {
    ++written_;
  }
}

void Snapshotter::tick(const Core &core, std::uint32_t frame) {
  if (!configured_) {
    configure();
  }
  if (g_sigRequested != 0) {
    g_sigRequested = 0;
    capture(core, frame, "SIGUSR1");
  } else if ((every_ != 0 && frame % every_ == 0) || at_.contains(frame)) {
    capture(core, frame, "scheduled");
  }
}

} // namespace psx::debug

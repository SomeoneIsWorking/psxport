// snapshot.cpp — on-demand guest RAM capture. See snapshot.h for the why.
#include "snapshot.h"
#include "core.h"
#include "diagnostic_config.h"
#include "fs_util.h"
#include "number_text.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <lucent/log.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

// Set from a signal handler, so it must be exactly this type — nothing else is safe to touch there.
volatile sig_atomic_t g_sig_requested = 0;

void on_usr1(int) {
  g_sig_requested = 1;
}

void parseList(const char *s, std::vector<uint64_t> &out) {
  if (!s) {
    return;
  }
  char buf[512];
  snprintf(buf, sizeof buf, "%s", s);
  for (char *t = strtok(buf, ","); t; t = strtok(nullptr, ",")) {
    out.push_back(strtoull(t, nullptr, 0));
  }
}

} // namespace

namespace psx::debug {

void Snapshotter::configure() {
  if (configured_) {
    return;
  }
  configured_ = true;
  const std::string frames = psx::config::snapshot_frames();
  parseList(frames.empty() ? nullptr : frames.c_str(), at_);
  if (const std::string every = psx::config::snapshot_every(); !every.empty()) {
    every_ = strtoull(every.c_str(), nullptr, 0);
  }
  if (const std::string max = psx::config::snapshot_max(); !max.empty()) {
    max_ = psx::config::parseInt(max).value_or(0);
  }
  // SIGUSR1 is installed unconditionally: it costs nothing, and the whole point is to be able to
  // snapshot a run that was NOT started with any snapshot flag set — which is every interesting run,
  // because you rarely know in advance that a state is worth capturing.
  signal(SIGUSR1, on_usr1);
  if (!at_.empty() || every_) {
    lucent::info("snap",
                 "scheduled: {} explicit frame(s), every={}, max={} "
                 "(also: kill -USR1 {})",
                 at_.size(),
                 (unsigned long long)every_,
                 max_,
                 (int)getpid());
  }
}

bool Snapshotter::captureNow(Core &core, const char *why) {
  if (written_ >= max_) {
    lucent::warn("snap",
                 "cap reached ({}) — NOT writing '{}'. Raise PSXPORT_SNAP_MAX if you meant it; "
                 "silently skipping would leave you waiting for a file that never arrives.",
                 max_,
                 why ? why : "?");
    return false;
  }
  char path[160], side[192];
  snprintf(path, sizeof path, "scratch/raw/snap_%llu.bin", (unsigned long long)frame_);
  snprintf(side, sizeof side, "%s.txt", path);
  if (!Fs::writeFile(path, core.ram, 0x200000)) {
    lucent::error("snap", "FAILED to write {} — the capture you asked for does not exist", path);
    return false;
  }
  // A directory of anonymous 2 MB dumps is unusable a day later. Say what this one is.
  std::string meta;
  char line[256];
  snprintf(line, sizeof line, "frame=%llu\nreason=%s\n", (unsigned long long)frame_, why ? why : "?");
  meta += line;
  snprintf(line,
           sizeof line,
           "base=0x80000000  size=0x200000\n"
           "import: use the maintained Ghidra workflow at base 0x80000000 for %s\n"
           "query : use the title-owned memory query tool with %s and a guest address\n",
           path,
           path);
  meta += line;
  Fs::writeFile(side, meta.data(), meta.size());
  written_++;
  lucent::info("snap", "frame {} -> {} ({})", (unsigned long long)frame_, path, why ? why : "?");
  return true;
}

void Snapshotter::tick(Core &core) {
  configure();
  frame_++;
  if (g_sig_requested) {
    g_sig_requested = 0;
    captureNow(core, "SIGUSR1");
    return;
  }
  if (every_ && (frame_ % every_) == 0) {
    captureNow(core, "PSXPORT_SNAP_EVERY");
    return;
  }
  for (uint64_t f : at_) {
    if (f == frame_) {
      captureNow(core, "PSXPORT_SNAP_AT");
      return;
    }
  }
}

} // namespace psx::debug

// snapshot.cpp — on-demand guest RAM capture. See snapshot.h for the why.
#include "snapshot.h"
#include "core.h"
#include "cfg.h"
#include "fs_util.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Set from a signal handler, so it must be exactly this type — nothing else is safe to touch there.
volatile sig_atomic_t g_sig_requested = 0;

void on_usr1(int) { g_sig_requested = 1; }

struct Sched {
  bool init = false;
  std::vector<uint64_t> at;     // explicit frame numbers
  uint64_t every = 0;           // periodic interval, 0 = off
  int max = 8;                  // hard cap on images per run
  int written = 0;
  uint64_t frame = 0;
};

Sched& sched() { static Sched s; return s; }

void parse_list(const char* s, std::vector<uint64_t>& out) {
  if (!s) return;
  char buf[512];
  snprintf(buf, sizeof buf, "%s", s);
  for (char* t = strtok(buf, ","); t; t = strtok(nullptr, ","))
    out.push_back(strtoull(t, nullptr, 0));
}

void init_once() {
  Sched& s = sched();
  if (s.init) return;
  s.init = true;
  parse_list(cfg_str("PSXPORT_SNAP_AT"), s.at);
  if (const char* e = cfg_str("PSXPORT_SNAP_EVERY")) s.every = strtoull(e, nullptr, 0);
  if (const char* m = cfg_str("PSXPORT_SNAP_MAX")) s.max = atoi(m);
  // SIGUSR1 is installed unconditionally: it costs nothing, and the whole point is to be able to
  // snapshot a run that was NOT started with any snapshot flag set — which is every interesting run,
  // because you rarely know in advance that a state is worth capturing.
  signal(SIGUSR1, on_usr1);
  if (!s.at.empty() || s.every)
    cfg_logi("snap", "scheduled: %zu explicit frame(s), every=%llu, max=%d "
                     "(also: kill -USR1 %d)",
             s.at.size(), (unsigned long long)s.every, s.max, (int)getpid());
}

}  // namespace

bool snapshot_now(Core* c, const char* why) {
  Sched& s = sched();
  if (s.written >= s.max) {
    cfg_logw("snap", "cap reached (%d) — NOT writing '%s'. Raise PSXPORT_SNAP_MAX if you meant it; "
                     "silently skipping would leave you waiting for a file that never arrives.",
             s.max, why ? why : "?");
    return false;
  }
  char path[160], side[192];
  snprintf(path, sizeof path, "scratch/raw/snap_%llu.bin", (unsigned long long)s.frame);
  snprintf(side, sizeof side, "%s.txt", path);
  if (!Fs::writeFile(path, c->ram, 0x200000)) {
    cfg_loge("snap", "FAILED to write %s — the capture you asked for does not exist", path);
    return false;
  }
  // A directory of anonymous 2 MB dumps is unusable a day later. Say what this one is.
  std::string meta;
  char line[256];
  snprintf(line, sizeof line, "frame=%llu\nreason=%s\n", (unsigned long long)s.frame,
           why ? why : "?");
  meta += line;
  snprintf(line, sizeof line, "base=0x80000000  size=0x200000\n"
                              "import: external/psxport/tools/decomp.sh import %s <proj>\n"
                              "query : tools/whatis.py --ram %s 0x800xxxxx\n", path, path);
  meta += line;
  Fs::writeFile(side, meta.data(), meta.size());
  s.written++;
  cfg_logi("snap", "frame %llu -> %s (%s)", (unsigned long long)s.frame, path, why ? why : "?");
  return true;
}

void snapshot_tick(Core* c) {
  init_once();
  Sched& s = sched();
  s.frame++;
  if (g_sig_requested) {
    g_sig_requested = 0;
    snapshot_now(c, "SIGUSR1");
    return;
  }
  if (s.every && (s.frame % s.every) == 0) {
    snapshot_now(c, "PSXPORT_SNAP_EVERY");
    return;
  }
  for (uint64_t f : s.at)
    if (f == s.frame) {
      snapshot_now(c, "PSXPORT_SNAP_AT");
      return;
    }
}

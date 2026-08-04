// hostprof.cpp — sampling profiler over the HOST program counter, to answer "which recompiled
// function actually runs hot".
//
// WHY NOT THE EXISTING ONE. interp_diag.h's profiler counts INTERPRETED functions, and a port with
// the interpreter removed (every un-owned function is recompiled C, a miss fail-fasts) has none to
// count. It would report nothing and that nothing would look like an answer.
//
// WHY NOT STATIC CALLER COUNTS. Choosing what to own natively has so far been driven by counting
// `jal` sites in the image. That is a proxy with two known holes: it cannot see indirect calls, and
// a function called from 136 places may still run less often than one called from 3 inside a loop.
// "The high-caller queue is exhausted" is a statement about the disassembly, not about the running
// port, and acting further on it without measuring would be inferring where measuring is available.
//
// WHAT THIS DOES. setitimer(ITIMER_PROF) + SIGPROF, sampling the host instruction pointer out of the
// signal ucontext. Each recompiled guest function is a real C symbol (gen_func_<ADDR>), so a
// histogram of host PCs resolves directly to guest functions offline — tools/prof_hot.py does the
// symbol mapping with nm, which keeps this file free of any symbol-table parsing.
//
//   PSXPORT_PROF=1            sample at the default 1 kHz
//   PSXPORT_PROF_HZ=<n>       change the rate
//   PSXPORT_PROF_OUT=<path>   default scratch/raw/prof_host.txt
//
// ITIMER_PROF counts CPU time, not wall time, so a port blocked on I/O or sleeping in frame pacing
// does not accumulate samples there — which is what you want when the question is "where is the work".
#include "cfg.h"
#include "fs_util.h"
#include <lucent/log.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/time.h>
#include <ucontext.h>

namespace {

// Open-addressed PC histogram, fixed size, touched only from a signal handler — so no allocation, no
// locking, and nothing that can deadlock if the sample lands inside malloc.
constexpr int PROF_SLOTS = 1 << 16;
struct Slot { uintptr_t pc; uint64_t n; };
Slot g_slots[PROF_SLOTS];
volatile sig_atomic_t g_on = 0;
uint64_t g_total = 0, g_dropped = 0;

void on_prof(int, siginfo_t*, void* uc) {
  if (!g_on) return;
#if defined(__x86_64__)
  const uintptr_t pc = (uintptr_t)((ucontext_t*)uc)->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
  const uintptr_t pc = (uintptr_t)((ucontext_t*)uc)->uc_mcontext.pc;
#else
  const uintptr_t pc = 0;
#endif
  if (!pc) return;
  g_total++;
  size_t h = (size_t)((pc >> 4) * 1147ull) & (PROF_SLOTS - 1);
  for (int probe = 0; probe < 64; probe++) {
    Slot& s = g_slots[(h + probe) & (PROF_SLOTS - 1)];
    if (s.pc == pc) { s.n++; return; }
    if (s.pc == 0) { s.pc = pc; s.n = 1; return; }
  }
  g_dropped++;   // table congested at this bucket — counted, never silently discarded
}

void dump();

// A profiled run is normally ENDED BY A SIGNAL — this port is killed by a timeout, not exited. So
// atexit() alone never fires and the profile silently does not exist, which is how the first run of
// this profiler produced nothing at all. Handle the terminating signals, dump, then re-raise with the
// default disposition so the exit status still reports what killed us (the gate depends on that).
void on_term(int sig) {
  dump();
  signal(sig, SIG_DFL);
  raise(sig);
}

void dump() {
  if (!g_on) return;
  g_on = 0;
  const char* out = cfg_str("PSXPORT_PROF_OUT");
  if (!out) out = "scratch/raw/prof_host.txt";
  std::string t;
  char line[128];
  snprintf(line, sizeof line, "# host-PC samples=%llu dropped=%llu\n",
           (unsigned long long)g_total, (unsigned long long)g_dropped);
  t += line;
  for (int i = 0; i < PROF_SLOTS; i++)
    if (g_slots[i].pc) {
      snprintf(line, sizeof line, "%016llx %llu\n",
               (unsigned long long)g_slots[i].pc, (unsigned long long)g_slots[i].n);
      t += line;
    }
  if (Fs::writeFile(out, t.data(), t.size()))
    lucent::info("prof", "{} sample(s) -> {}  (resolve with tools/prof_hot.py)",
                 (unsigned long long)g_total, out);
  else
    lucent::error("prof", "FAILED to write {} — the profile you asked for does not exist", out);
  if (g_dropped)
    lucent::warn("prof", "{} sample(s) DROPPED to table congestion — the hot set is wider than the "
                         "histogram; treat the tail as incomplete", (unsigned long long)g_dropped);
}

}  // namespace

void hostprof_init() {
  if (!cfg_on("PSXPORT_PROF")) return;
  int hz = 1000;
  if (const char* h = cfg_str("PSXPORT_PROF_HZ")) hz = atoi(h);
  if (hz < 1) hz = 1;

  struct sigaction sa {};
  sa.sa_sigaction = on_prof;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGPROF, &sa, nullptr);

  struct itimerval it {};
  it.it_interval.tv_usec = 1000000 / hz;
  it.it_value = it.it_interval;
  setitimer(ITIMER_PROF, &it, nullptr);

  g_on = 1;
  atexit(dump);
  // SIGTERM/SIGINT are how a profiled run actually ends here. SIGPROF stays on its own handler.
  signal(SIGTERM, on_term);
  signal(SIGINT, on_term);
  lucent::info("prof", "host-PC sampling at {} Hz (ITIMER_PROF — CPU time, not wall time)", hz);
}

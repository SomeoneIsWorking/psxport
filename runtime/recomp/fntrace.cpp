// fntrace.cpp — PSXPORT_FNTRACE=<addr>[,<addr>...]: did control REACH this guest function, and from where?
//
// THE QUESTION THIS EXISTS FOR. Reverse-engineering a big guest function repeatedly runs into "which
// of these branches actually executes?", and nothing in this port answered it. Watchpoints only reveal
// blocks that STORE to a watched address. hostprof resolves host PCs to whole functions, so every
// block of a 5000-instruction handler looks the same. That gap cost a real mistake in the consuming
// port (issue 0027): a branch gate was analysed in detail, twice, before a watchpoint happened to show
// that the code containing it never ran at all. Analysing dead code reads exactly like analysing live
// code — there is no symptom.
//
// WHY A FUNCTION TRACER ANSWERS A BLOCK QUESTION. Distinct paths through a big function almost always
// make distinct CALLS, and every call to a recompiled entry goes through the generated dispatcher. So
// "was 0x8001277C called?" stands in for "did the block ending in that call run?" — with no recompiler
// change, no per-block instrumentation, and no cost on any run that does not ask for it. Pick a callee
// that is UNIQUE to the path you are asking about; a callee shared with other paths answers nothing.
//
// HOW IT RUNS THE REAL BODY. The generated dispatcher is `pc = addr; if (override) { override(c);
// return; } gen(c);`, so an override cannot super-call without naming the generated symbol. Instead the
// hook clears ITSELF, re-dispatches (which now finds no override and runs the real body), then puts
// itself back. Behaviour is unchanged; only the log is added.
//
// THREE LIMITS, STATED BECAUSE A SILENT ONE WOULD BE WORSE THAN THE GAP:
//   * IT CLAIMS THE OVERRIDE SLOT, so tracing an address the port already overrides REPLACES that
//     override for the run. The first version of this file installed before the game's registrations
//     and was silently displaced by them: it reported "NEVER CALLED" for the CD loader, which runs 14
//     times a boot. That is the worst failure a tracer can have, so init now runs LAST and wins. The
//     cost is the mirror hazard: do NOT trace an address whose override DOES work (the game's CD
//     loader serves disc data — replacing it stops loading). Tracing a VERIFIED native body is
//     harmless, since re-dispatch runs the recompiled body the differential proved equivalent.
//   * MAIN-module entries only. Overlay modules expose disp/idx through RecompRegistry but no
//     per-overlay override setter, so an overlay address cannot be hooked this way. Trace a MAIN
//     function the overlay path calls instead — which is the common case anyway.
//   * RECURSION IS NOT COUNTED. The hook is uninstalled while the body runs, so a function that calls
//     itself logs the outer call only. That is a deliberate trade for keeping the body's behaviour
//     byte-identical; if you need recursive counts, this is the wrong instrument.
//
// A ZERO COUNT IS A REAL ANSWER, and the summary says so explicitly rather than printing nothing —
// "never called" is usually the finding you are after, and an empty log is indistinguishable from a
// tracer that was never armed.
#include "cfg.h"
#include "core.h"
#include "recomp_iface.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>

// Declared at file scope: inside the anonymous namespace this would get internal linkage and fail to
// resolve against the real definition.
int gpu_frame_no(Core*);

namespace {

constexpr int MAX_TRACE = 16;
struct Site { uint32_t addr; unsigned long hits; uint32_t first_ra; int first_frame; };
Site g_sites[MAX_TRACE];
int g_n = 0;
bool g_on = false;

int find(uint32_t a) {
  for (int i = 0; i < g_n; i++)
    if (g_sites[i].addr == a) return i;
  return -1;
}

void hook(Core* c) {
  // The dispatcher sets c->pc = addr immediately before consulting the override slot, so this is the
  // address we were installed at. Validated rather than assumed: if it is not one of ours, something
  // about that contract changed and every number below would be attributed to the wrong function.
  const uint32_t addr = c->pc;
  const int i = find(addr);
  if (i < 0) {
    cfg_loge("fntrace", "hook fired with pc=%08X, which is not a traced address — the dispatcher's "
                        "pc-before-override contract no longer holds; counts cannot be trusted", addr);
    return;
  }
  if (g_sites[i].hits == 0) {
    g_sites[i].first_ra = c->r[31];
    g_sites[i].first_frame = gpu_frame_no(c);
    cfg_logi("fntrace", "0x%08X REACHED — first call at frame %d from ra=%08X",
             addr, g_sites[i].first_frame, g_sites[i].first_ra);
  }
  g_sites[i].hits++;

  const RecompRegistry* R = psxport_recomp();
  R->shard_set_override(addr, nullptr);   // step aside so the dispatcher runs the real body
  R->main_dispatch(c, addr);
  R->shard_set_override(addr, hook);      // and back, for the next call
}

void report() {
  if (!g_on) return;
  for (int i = 0; i < g_n; i++) {
    if (g_sites[i].hits)
      cfg_logi("fntrace", "0x%08X: %lu call(s), first at frame %d from ra=%08X",
               g_sites[i].addr, g_sites[i].hits, g_sites[i].first_frame, g_sites[i].first_ra);
    else
      cfg_logi("fntrace", "0x%08X: NEVER CALLED — control did not reach it in this run",
               g_sites[i].addr);
  }
}

// A traced run normally ends by SIGNAL — the port is killed by a timeout, not exited — so atexit()
// alone never fires and the summary silently does not exist. That matters more here than for most
// tools: the summary is where "NEVER CALLED" is reported, and its absence looks identical to a run
// in which everything was called. Handle the terminating signals, report, then re-raise with the
// default disposition so the exit status still says what killed us.
void on_term(int sig) {
  report();
  signal(sig, SIG_DFL);
  raise(sig);
}

}  // namespace

void fntrace_init() {
  const char* s = cfg_str("PSXPORT_FNTRACE");
  if (!s || !*s) return;
  const RecompRegistry* R = psxport_recomp();
  if (!R || !R->shard_set_override || !R->main_dispatch) {
    cfg_loge("fntrace", "recomp registry not installed yet — call fntrace_init() after it");
    return;
  }
  char buf[512];
  snprintf(buf, sizeof buf, "%s", s);
  for (char* tok = strtok(buf, ", "); tok && g_n < MAX_TRACE; tok = strtok(nullptr, ", ")) {
    const uint32_t a = (uint32_t)strtoul(tok, nullptr, 16);
    if (!a) continue;
    if (R->rec_func_index && R->rec_func_index(a) < 0) {
      // Not a MAIN entry: either an overlay address (unhookable here) or not a function start at all.
      // Say which, because "my trace printed nothing" otherwise looks like "the code never ran".
      cfg_loge("fntrace", "0x%08X is not a MAIN function entry — overlay entries cannot be hooked "
                          "this way, and a mid-function address is not an entry at all. NOT traced.", a);
      continue;
    }
    g_sites[g_n].addr = a; g_sites[g_n].hits = 0;
    R->shard_set_override(a, hook);
    g_n++;
  }
  if (!g_n) return;
  g_on = true;
  atexit(report);
  signal(SIGTERM, on_term);
  signal(SIGINT, on_term);
  cfg_logi("fntrace", "tracing %d guest function(s); a zero count at exit means NEVER REACHED", g_n);
}

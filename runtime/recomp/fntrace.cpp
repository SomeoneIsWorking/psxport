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
// HOW IT PRESERVES THE SHIPPING OWNER. A generated raw-slot getter captures the exact handler already
// installed at each address. The hook temporarily restores and calls that handler, then puts itself
// back. Only a genuinely empty prior slot redispatches to the retained generated body. This is raw-slot
// ownership, not a second interpretation of the title/PlatformHle registries.
//
// THREE LIMITS, STATED BECAUSE A SILENT ONE WOULD BE WORSE THAN THE GAP:
//   * IT REQUIRES A GENERATED RAW-SLOT GETTER. Old generated substrates expose only a setter, so the
//     tracer cannot know whether null redispatch would bypass a native title/PlatformHle owner. It
//     loudly refuses to arm until those substrates are regenerated; it never guesses.
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
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <lucent/log.h>

// Declared at file scope: inside the anonymous namespace this would get internal linkage and fail to
// resolve against the real definition.
int gpu_frame_no(Core *);

namespace {

constexpr int MAX_TRACE = 16;
struct Site {
  uint32_t addr;
  unsigned long hits;
  uint32_t first_ra;
  int first_frame;
  int abi_reported;
  unsigned long abi_violations;
  RecOverrideFn prior;
};

// Callee-saved under the MIPS o32 ABI: a function that returns with any of these changed has broken
// its contract with every caller. A guest compiler does not emit such code, so a mismatch here means
// the RECOMPILATION of that function is wrong — which is otherwise almost impossible to see, because
// the damage lands in the caller's locals and surfaces far away as a loop that will not terminate.
const int CALLEE_SAVED[] = {16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31}; // s0-s7, gp, sp, fp, ra
const char *const CALLEE_SAVED_NAME[] = {"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "gp", "sp", "fp", "ra"};
Site g_sites[MAX_TRACE];
int g_n = 0;
long g_regs_n = 0; // PSXPORT_FNTRACE_REGS: how many calls per site to dump registers for
bool g_on = false;
bool g_initialized = false;

int find(uint32_t a) {
  for (int i = 0; i < g_n; i++) {
    if (g_sites[i].addr == a) {
      return i;
    }
  }
  return -1;
}

void hook(Core *c) {
  // The dispatcher sets c->pc = addr immediately before consulting the override slot, so this is the
  // address we were installed at. Validated rather than assumed: if it is not one of ours, something
  // about that contract changed and every number below would be attributed to the wrong function.
  const uint32_t addr = c->pc;
  const int i = find(addr);
  if (i < 0) {
    lucent::error("fntrace",
                  "hook fired with pc={:08X}, which is not a traced address — the dispatcher's "
                  "pc-before-override contract no longer holds; counts cannot be trusted",
                  addr);
    return;
  }
  if (g_sites[i].hits == 0) {
    g_sites[i].first_ra = c->r[31];
    g_sites[i].first_frame = gpu_frame_no(c);
    lucent::info("fntrace",
                 "0x{:08X} REACHED — first call at frame {} from ra={:08X}",
                 addr,
                 g_sites[i].first_frame,
                 g_sites[i].first_ra);
  }
  g_sites[i].hits++;

  // PSXPORT_FNTRACE_REGS=N — dump the argument and saved registers for the first N calls of each
  // site. "Is this register what the static reading predicts?" is otherwise unanswerable: a
  // watchpoint shows memory, the ABI check shows preservation, and neither shows what a loop
  // counter actually HOLDS on entry.
  if (g_regs_n && (long)g_sites[i].hits <= g_regs_n) {
    lucent::info("fntrace",
                 "0x{:08X} call #{}: a0={:08X} a1={:08X} a2={:08X} a3={:08X} | s0={:08X} s1={:08X} s2={:08X} s3={:08X}",
                 addr,
                 g_sites[i].hits,
                 c->r[4],
                 c->r[5],
                 c->r[6],
                 c->r[7],
                 c->r[16],
                 c->r[17],
                 c->r[18],
                 c->r[19]);
  }

  // ABI CHECK. Snapshot the callee-saved registers, run the body, and compare. This is free to add
  // here because the trampoline already brackets the call, and it answers a question nothing else can:
  // "is this function's recompilation ABI-correct?". A violation corrupts the CALLER's locals, so it
  // surfaces as a bug with no visible connection to the culprit.
  uint32_t saved[sizeof CALLEE_SAVED / sizeof CALLEE_SAVED[0]];
  for (size_t k = 0; k < sizeof saved / sizeof saved[0]; k++) {
    saved[k] = c->r[CALLEE_SAVED[k]];
  }

  const RecompRegistry *R = psxport_recomp();
  R->shard_set_override(addr, g_sites[i].prior); // step aside for the exact shipping owner
  if (g_sites[i].prior) {
    g_sites[i].prior(c);
  } else {
    R->main_dispatch(c, addr); // an empty slot authoritatively means the retained generated body
  }
  R->shard_set_override(addr, hook); // and back, for the next call

  // PSXPORT_FNTRACE_SELFTEST=1 — deliberately clobber s0 after the call so the check below MUST fire.
  // A checker that reports "0 violations" everywhere looks identical to one that cannot report at all,
  // and the control case passing proves nothing. This is how to confirm it can say the other thing.
  if (cfg_on("PSXPORT_FNTRACE_SELFTEST")) {
    c->r[16] ^= 0xA5A5A5A5u;
  }

  // REPORT EVERY DIFFERING REGISTER, not just the first. The earlier version stopped at one, and
  // that under-report cost real time: 0x80047B60 was reported as clobbering s0 (index 0) while sp
  // and ra were mangled by the same missing epilogue and stayed invisible, which made the failure
  // look narrower — and stranger — than it was. Seeing sp move by the frame size would have named
  // "the epilogue did not run" immediately instead of "something ate a register". Still first CALL
  // only: a broken function breaks on every call, and 62M identical lines help nobody.
  int listed = 0;
  for (size_t k = 0; k < sizeof saved / sizeof saved[0]; k++) {
    if (c->r[CALLEE_SAVED[k]] == saved[k]) {
      continue;
    }
    g_sites[i].abi_violations++;
    if (g_sites[i].abi_reported) {
      continue;
    }
    if (listed++ == 0) {
      lucent::error("fntrace",
                    "0x{:08X} VIOLATES THE ABI. A guest compiler does not emit this, so the "
                    "RECOMPILATION is wrong; the damage lands in the caller's locals, far "
                    "from here. Every register that moved:",
                    addr);
    }
    lucent::error("fntrace",
                  "    {:<2} entered as {:08X}, returned as {:08X}{}",
                  CALLEE_SAVED_NAME[k],
                  saved[k],
                  c->r[CALLEE_SAVED[k]],
                  CALLEE_SAVED[k] == 29 ? "   (sp moved: an epilogue did not run)" : "");
  }
  if (listed) {
    g_sites[i].abi_reported = 1;
  }
}

void report() {
  if (!g_on) {
    return;
  }
  for (int i = 0; i < g_n; i++) {
    if (g_sites[i].hits) {
      lucent::info("fntrace",
                   "0x{:08X}: {} call(s), first at frame {} from ra={:08X}, ABI violations {}",
                   g_sites[i].addr,
                   g_sites[i].hits,
                   g_sites[i].first_frame,
                   g_sites[i].first_ra,
                   g_sites[i].abi_violations);
    } else {
      lucent::info("fntrace", "0x{:08X}: NEVER CALLED — control did not reach it in this run", g_sites[i].addr);
    }
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

} // namespace

void fntrace_init() {
  // Registration is process-global, while dc_boot_init may run once per Core. Parse the requested
  // sites and install reporting exactly once, but re-apply the hooks after each game's override
  // registration so a later Core cannot silently displace the diagnostic.
  const bool first_init = !g_initialized;
  if (const char *r = cfg_str("PSXPORT_FNTRACE_REGS")) {
    g_regs_n = atol(r);
  }
  const char *s = first_init ? cfg_str("PSXPORT_FNTRACE") : nullptr;
  if (first_init && (!s || !*s)) {
    g_initialized = true;
    return;
  }
  if (!first_init && !g_n) {
    return;
  }
  const RecompRegistry *R = psxport_recomp();
  if (!R || !R->shard_set_override || !R->main_dispatch) {
    lucent::error("fntrace", "recomp registry not installed yet — call fntrace_init() after it");
    return;
  }
  if (!R->shard_get_override) {
    lucent::error("fntrace",
                  "generated substrate has no raw override getter — FNTRACE is unavailable until "
                  "the title is regenerated; no hooks were installed");
    return;
  }
  g_initialized = true;
  if (first_init) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *tok = strtok(buf, ", "); tok && g_n < MAX_TRACE; tok = strtok(nullptr, ", ")) {
      const uint32_t a = (uint32_t)strtoul(tok, nullptr, 16);
      if (!a) {
        continue;
      }
      if (R->rec_func_index && R->rec_func_index(a) < 0) {
        // Not a MAIN entry: either an overlay address (unhookable here) or not a function start at all.
        // Say which, because "my trace printed nothing" otherwise looks like "the code never ran".
        lucent::error("fntrace",
                      "0x{:08X} is not a MAIN function entry — overlay entries cannot be hooked "
                      "this way, and a mid-function address is not an entry at all. NOT traced.",
                      a);
        continue;
      }
      g_sites[g_n].addr = a;
      g_sites[g_n].hits = 0;
      ++g_n;
    }
  }
  if (!g_n) {
    return;
  }
  for (int i = 0; i < g_n; ++i) {
    const RecOverrideFn current = R->shard_get_override(g_sites[i].addr);
    // A repeated init sees our own hook on sites that no later registration touched. Retain the
    // original owner there; otherwise capture the exact newly installed owner before taking back
    // the diagnostic slot.
    if (current != hook) {
      g_sites[i].prior = current;
    }
    R->shard_set_override(g_sites[i].addr, hook);
  }
  if (!first_init) {
    return;
  }
  g_on = true;
  atexit(report);
  signal(SIGTERM, on_term);
  signal(SIGINT, on_term);
  lucent::info("fntrace", "tracing {} guest function(s); a zero count at exit means NEVER REACHED", g_n);
}

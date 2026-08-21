// native_diff.cpp — per-call differential between a native body and the recompiled one. See the header.
#include "native_diff.h"
#include "cfg.h"
#include "core.h"
#include "game.h" // Game::gte — the COP2 register file, compared below
#include <cstdlib>
#include <cstring>
#include <deque>
#include <lucent/log.h>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr uint32_t RAM_SIZE = 0x200000;

struct Site {
  int calls = 0;
  int diffs = 0;
};

struct SnapshotFrame {
  // Reused at each nesting depth so a verified run does not churn 6 MB of allocation per call.
  std::vector<uint8_t> preRam, nativeRam, substrateRam;
  std::vector<uint8_t> preSpad, nativeSpad, substrateSpad;
  GteRegs preGte{}, nativeGte{}, substrateGte{};
};

struct State {
  bool init = false;
  int budget = 0;   // calls to verify per site; 0 = disabled
  int maxdiff = 32; // per-call cap on individual byte diffs listed
  int divergences = 0;
  std::map<std::string, Site> sites;
  // A parent comparison can call another owned body, which opens a nested ndiff_run. Each active
  // depth needs an independent snapshot; a singleton lets the child overwrite the parent's rewind
  // state and native answer. deque keeps the outer frame reference stable when a deeper frame is
  // first allocated, and completed frames retain their vector capacity for the next call.
  std::deque<SnapshotFrame> frames;
};

State &st() {
  static State s;
  return s;
}

void init_once() {
  State &s = st();
  if (s.init) {
    return;
  }
  s.init = true;
  if (const char *n = cfg_str("PSXPORT_NDIFF")) {
    s.budget = atoi(n);
  }
  if (const char *m = cfg_str("PSXPORT_NDIFF_MAXDIFF")) {
    s.maxdiff = atoi(m);
  }
  if (s.budget > 0) {
    lucent::info("ndiff",
                 "per-call differential ON: verifying the first {} call(s) of each native site "
                 "against the recompiled body",
                 s.budget);
  }
}

// Report the differing bytes, bounded. A wall of diffs is not more informative than the first few
// plus a count, and an unbounded dump buries the one address that matters.
int report_ram(const char *name,
               const std::vector<uint8_t> &a,
               const std::vector<uint8_t> &b,
               uint32_t base,
               const char *what,
               int cap) {
  int shown = 0, total = 0;
  for (size_t i = 0; i < a.size(); i++) {
    if (a[i] == b[i]) {
      continue;
    }
    total++;
    if (shown < cap) {
      lucent::error("ndiff",
                    "  {} {} 0x{:08X}: native={:02X} substrate={:02X}",
                    name ? name : "(null)",
                    what,
                    (unsigned)(base + i),
                    a[i],
                    b[i]);
      shown++;
    }
  }
  if (total > shown) {
    lucent::error(
        "ndiff", "  {} {}: {} more differing byte(s) not listed", name ? name : "(null)", what, total - shown);
  }
  return total;
}

} // namespace

int ndiff_divergences() {
  return st().divergences;
}

// Depth rather than a bool: ndiff_run's substrate leg can reach another natively-owned body, which
// opens a second window inside the first. A bool would close on the inner one's exit and re-open the
// outer leg to injection for the rest of its run.
static int s_in_diff = 0;
bool ndiff_in_progress() {
  return s_in_diff > 0;
}

bool ndiff_run(Core *c, const char *name, void (*native)(Core *), void (*body)(Core *)) {
  init_once();
  State &s = st();

  // DISABLED IS THE COMMON CASE, AND IT MUST COST NOTHING. The site lookup used to happen before the
  // budget test, so every call of every natively-owned body paid for a std::string constructed from
  // `name` plus a red-black tree walk — on a run with the differential switched off, where none of that
  // bookkeeping is ever read. With 18 owned bodies on hot paths that showed up as 9.7% of total CPU in
  // a host profile, which is a tenth of the port spent maintaining a map nobody queries. Test the
  // budget first and the disabled path touches no container at all.
  if (s.budget <= 0) {
    native(c);
    return false;
  }
  Site &site = s.sites[name];
  if (site.calls >= s.budget) {
    site.calls++;
    native(c);
    return false;
  }
  site.calls++;

  const size_t depth = static_cast<size_t>(s_in_diff);
  if (s.frames.size() <= depth) {
    s.frames.resize(depth + 1u);
  }
  SnapshotFrame &snapshot = s.frames[depth];

  const size_t spad = sizeof c->scratch;
  // Both legs run inside this window; see ndiff_in_progress() in the header for what must stand
  // still while they do. RAII so an early return or a throw cannot leave the port permanently unable
  // to take a host turn.
  struct DiffWindow {
    DiffWindow() {
      ++s_in_diff;
    }
    ~DiffWindow() {
      --s_in_diff;
    }
  } _diff_window;

  snapshot.preRam.assign(c->ram, c->ram + RAM_SIZE);
  snapshot.preSpad.assign(c->scratch, c->scratch + spad);
  const R3000 preRegs = *static_cast<R3000 *>(c);
  snapshot.preGte = c->game->gte;

  native(c);
  snapshot.nativeRam.assign(c->ram, c->ram + RAM_SIZE);
  snapshot.nativeSpad.assign(c->scratch, c->scratch + spad);
  const R3000 natRegs = *static_cast<R3000 *>(c);
  snapshot.nativeGte = c->game->gte;

  // Rewind to the exact pre-state and run the body the native code claims to replace.
  memcpy(c->ram, snapshot.preRam.data(), RAM_SIZE);
  memcpy(c->scratch, snapshot.preSpad.data(), spad);
  *static_cast<R3000 *>(c) = preRegs;
  c->game->gte = snapshot.preGte;
  body(c);
  snapshot.substrateRam.assign(c->ram, c->ram + RAM_SIZE);
  snapshot.substrateSpad.assign(c->scratch, c->scratch + spad);
  const R3000 subRegs = *static_cast<R3000 *>(c);
  snapshot.substrateGte = c->game->gte;

  int diffs = 0;
  diffs += report_ram(name, snapshot.nativeRam, snapshot.substrateRam, 0x80000000u, "RAM", s.maxdiff);
  diffs += report_ram(name, snapshot.nativeSpad, snapshot.substrateSpad, 0x1F800000u, "scratchpad", s.maxdiff);
  // v0/v1 are the return values and the usual place a contract mismatch shows; report every GPR that
  // differs, since "which register" is most of the diagnosis.
  static const char *kReg[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                 "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                 "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
  for (int i = 1; i < 32; i++) {
    if (natRegs.r[i] != subRegs.r[i]) {
      lucent::error("ndiff",
                    "  {} reg {}: native=0x{:08X} substrate=0x{:08X}",
                    name ? name : "(null)",
                    kReg[i],
                    natRegs.r[i],
                    subRegs.r[i]);
      diffs++;
    }
  }
  // COP2 data regs are DR = REG[0..31], control regs CR = REG[32..63]; name them that way so a diff
  // reads as "cop2 DR12" rather than an opaque index.
  for (int i = 0; i < 64; i++) {
    if (snapshot.nativeGte.REG[i] != snapshot.substrateGte.REG[i]) {
      lucent::error("ndiff",
                    "  {} cop2 {}{}: native=0x{:08X} substrate=0x{:08X}",
                    name ? name : "(null)",
                    i < 32 ? "DR" : "CR",
                    i < 32 ? i : i - 32,
                    snapshot.nativeGte.REG[i],
                    snapshot.substrateGte.REG[i]);
      diffs++;
    }
  }
  if (snapshot.nativeGte.FLAGS != snapshot.substrateGte.FLAGS) {
    lucent::error("ndiff",
                  "  {} cop2 FLAGS: native=0x{:08X} substrate=0x{:08X}",
                  name ? name : "(null)",
                  snapshot.nativeGte.FLAGS,
                  snapshot.substrateGte.FLAGS);
    diffs++;
  }
  if (natRegs.hi != subRegs.hi || natRegs.lo != subRegs.lo) {
    lucent::error("ndiff",
                  "  {} hi/lo: native={:08X}/{:08X} substrate={:08X}/{:08X}",
                  name ? name : "(null)",
                  natRegs.hi,
                  natRegs.lo,
                  subRegs.hi,
                  subRegs.lo);
    diffs++;
  }

  // LEAVE THE NATIVE RESULT IN PLACE. Falling back to the substrate on a diff would make a broken
  // replacement behave correctly under the very flag meant to expose it.
  memcpy(c->ram, snapshot.nativeRam.data(), RAM_SIZE);
  memcpy(c->scratch, snapshot.nativeSpad.data(), spad);
  *static_cast<R3000 *>(c) = natRegs;
  c->game->gte = snapshot.nativeGte;

  if (diffs) {
    site.diffs++;
    s.divergences++;
    lucent::error("ndiff",
                  "{} call #{} DIVERGES from the recompiled body ({} difference(s))",
                  name ? name : "(null)",
                  site.calls,
                  diffs);
  } else {
    lucent::info("ndiff", "{} call #{} matches the recompiled body exactly", name ? name : "(null)", site.calls);
  }
  return diffs != 0;
}

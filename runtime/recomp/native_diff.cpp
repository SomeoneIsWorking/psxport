// native_diff.cpp — per-call differential between a native body and the recompiled one. See the header.
#include "native_diff.h"
#include "core.h"
#include "cfg.h"
#include <lucent/log.h>
#include "game.h"      // Game::gte — the COP2 register file, compared below
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr uint32_t RAM_SIZE = 0x200000;

struct Site { int calls = 0; int diffs = 0; };

struct State {
  bool init = false;
  int budget = 0;          // calls to verify per site; 0 = disabled
  int maxdiff = 32;        // per-call cap on individual byte diffs listed
  int divergences = 0;
  std::map<std::string, Site> sites;
  // Reused across calls so a verified run does not churn 6 MB of allocation per call.
  std::vector<uint8_t> pre, natRam, subRam;
  std::vector<uint8_t> preSpad, natSpad, subSpad;
  // COP2/GTE. A PS1 game's hot maths lives here, and comparing only GPRs + RAM would report "matches"
  // while a native body silently left the GTE register file different — the substrate's `cop2`
  // instructions write REG[0..63] and FLAGS, none of which is guest RAM. Without this the differential
  // simply cannot verify any function containing lwc2/swc2/cop2, which is most of the geometry code.
  GteRegs preGte{}, natGte{}, subGte{};
};

State& st() { static State s; return s; }

void init_once() {
  State& s = st();
  if (s.init) return;
  s.init = true;
  if (const char* n = cfg_str("PSXPORT_NDIFF")) s.budget = atoi(n);
  if (const char* m = cfg_str("PSXPORT_NDIFF_MAXDIFF")) s.maxdiff = atoi(m);
  if (s.budget > 0)
    lucent::info("ndiff", "per-call differential ON: verifying the first {} call(s) of each native site "
                          "against the recompiled body", s.budget);
}

// Report the differing bytes, bounded. A wall of diffs is not more informative than the first few
// plus a count, and an unbounded dump buries the one address that matters.
int report_ram(const char* name, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
               uint32_t base, const char* what, int cap) {
  int shown = 0, total = 0;
  for (size_t i = 0; i < a.size(); i++) {
    if (a[i] == b[i]) continue;
    total++;
    if (shown < cap) {
      lucent::error("ndiff", "  {} {} 0x{:08X}: native={:02X} substrate={:02X}",
                    name ? name : "(null)", what, (unsigned)(base + i), a[i], b[i]);
      shown++;
    }
  }
  if (total > shown)
    lucent::error("ndiff", "  {} {}: {} more differing byte(s) not listed", name ? name : "(null)", what, total - shown);
  return total;
}

}  // namespace

int ndiff_divergences() { return st().divergences; }

// Depth rather than a bool: ndiff_run's substrate leg can reach another natively-owned body, which
// opens a second window inside the first. A bool would close on the inner one's exit and re-open the
// outer leg to injection for the rest of its run.
static int s_in_diff = 0;
bool ndiff_in_progress() { return s_in_diff > 0; }

bool ndiff_run(Core* c, const char* name, void (*native)(Core*), void (*body)(Core*)) {
  init_once();
  State& s = st();

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
  Site& site = s.sites[name];
  if (site.calls >= s.budget) {
    site.calls++;
    native(c);
    return false;
  }
  site.calls++;

  const size_t spad = sizeof c->scratch;
  // Both legs run inside this window; see ndiff_in_progress() in the header for what must stand
  // still while they do. RAII so an early return or a throw cannot leave the port permanently unable
  // to take a host turn.
  struct DiffWindow { DiffWindow() { ++s_in_diff; } ~DiffWindow() { --s_in_diff; } } _diff_window;

  s.pre.assign(c->ram, c->ram + RAM_SIZE);
  s.preSpad.assign(c->scratch, c->scratch + spad);
  const R3000 preRegs = *static_cast<R3000*>(c);
  s.preGte = c->game->gte;

  native(c);
  s.natRam.assign(c->ram, c->ram + RAM_SIZE);
  s.natSpad.assign(c->scratch, c->scratch + spad);
  const R3000 natRegs = *static_cast<R3000*>(c);
  s.natGte = c->game->gte;

  // Rewind to the exact pre-state and run the body the native code claims to replace.
  memcpy(c->ram, s.pre.data(), RAM_SIZE);
  memcpy(c->scratch, s.preSpad.data(), spad);
  *static_cast<R3000*>(c) = preRegs;
  c->game->gte = s.preGte;
  body(c);
  s.subRam.assign(c->ram, c->ram + RAM_SIZE);
  s.subSpad.assign(c->scratch, c->scratch + spad);
  const R3000 subRegs = *static_cast<R3000*>(c);
  s.subGte = c->game->gte;

  int diffs = 0;
  diffs += report_ram(name, s.natRam, s.subRam, 0x80000000u, "RAM", s.maxdiff);
  diffs += report_ram(name, s.natSpad, s.subSpad, 0x1F800000u, "scratchpad", s.maxdiff);
  // v0/v1 are the return values and the usual place a contract mismatch shows; report every GPR that
  // differs, since "which register" is most of the diagnosis.
  static const char* kReg[32] = {
    "zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3","t4","t5","t6","t7",
    "s0","s1","s2","s3","s4","s5","s6","s7","t8","t9","k0","k1","gp","sp","fp","ra"};
  for (int i = 1; i < 32; i++)
    if (natRegs.r[i] != subRegs.r[i]) {
      lucent::error("ndiff", "  {} reg {}: native=0x{:08X} substrate=0x{:08X}", name ? name : "(null)", kReg[i],
                    natRegs.r[i], subRegs.r[i]);
      diffs++;
    }
  // COP2 data regs are DR = REG[0..31], control regs CR = REG[32..63]; name them that way so a diff
  // reads as "cop2 DR12" rather than an opaque index.
  for (int i = 0; i < 64; i++)
    if (s.natGte.REG[i] != s.subGte.REG[i]) {
      lucent::error("ndiff", "  {} cop2 {}{}: native=0x{:08X} substrate=0x{:08X}", name ? name : "(null)",
                    i < 32 ? "DR" : "CR", i < 32 ? i : i - 32, s.natGte.REG[i], s.subGte.REG[i]);
      diffs++;
    }
  if (s.natGte.FLAGS != s.subGte.FLAGS) {
    lucent::error("ndiff", "  {} cop2 FLAGS: native=0x{:08X} substrate=0x{:08X}", name ? name : "(null)",
                  s.natGte.FLAGS, s.subGte.FLAGS);
    diffs++;
  }
  if (natRegs.hi != subRegs.hi || natRegs.lo != subRegs.lo) {
    lucent::error("ndiff", "  {} hi/lo: native={:08X}/{:08X} substrate={:08X}/{:08X}", name ? name : "(null)",
                  natRegs.hi, natRegs.lo, subRegs.hi, subRegs.lo);
    diffs++;
  }

  // LEAVE THE NATIVE RESULT IN PLACE. Falling back to the substrate on a diff would make a broken
  // replacement behave correctly under the very flag meant to expose it.
  memcpy(c->ram, s.natRam.data(), RAM_SIZE);
  memcpy(c->scratch, s.natSpad.data(), spad);
  *static_cast<R3000*>(c) = natRegs;
  c->game->gte = s.natGte;

  if (diffs) {
    site.diffs++;
    s.divergences++;
    lucent::error("ndiff", "{} call #{} DIVERGES from the recompiled body ({} difference(s))",
                  name ? name : "(null)", site.calls, diffs);
  } else {
    lucent::info("ndiff", "{} call #{} matches the recompiled body exactly", name ? name : "(null)", site.calls);
  }
  return diffs != 0;
}

// native_diff.cpp — per-call differential between a native body and the recompiled one. See the header.
#include "native_diff.h"
#include "core.h"
#include "cfg.h"
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
};

State& st() { static State s; return s; }

void init_once() {
  State& s = st();
  if (s.init) return;
  s.init = true;
  if (const char* n = cfg_str("PSXPORT_NDIFF")) s.budget = atoi(n);
  if (const char* m = cfg_str("PSXPORT_NDIFF_MAXDIFF")) s.maxdiff = atoi(m);
  if (s.budget > 0)
    cfg_logi("ndiff", "per-call differential ON: verifying the first %d call(s) of each native site "
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
      cfg_loge("ndiff", "  %s %s 0x%08X: native=%02X substrate=%02X",
               name, what, (unsigned)(base + i), a[i], b[i]);
      shown++;
    }
  }
  if (total > shown)
    cfg_loge("ndiff", "  %s %s: %d more differing byte(s) not listed", name, what, total - shown);
  return total;
}

}  // namespace

int ndiff_divergences() { return st().divergences; }

bool ndiff_run(Core* c, const char* name, void (*native)(Core*), void (*body)(Core*)) {
  init_once();
  State& s = st();
  Site& site = s.sites[name];

  if (s.budget <= 0 || site.calls >= s.budget) {
    site.calls++;
    native(c);
    return false;
  }
  site.calls++;

  const size_t spad = sizeof c->scratch;
  s.pre.assign(c->ram, c->ram + RAM_SIZE);
  s.preSpad.assign(c->scratch, c->scratch + spad);
  const R3000 preRegs = *static_cast<R3000*>(c);

  native(c);
  s.natRam.assign(c->ram, c->ram + RAM_SIZE);
  s.natSpad.assign(c->scratch, c->scratch + spad);
  const R3000 natRegs = *static_cast<R3000*>(c);

  // Rewind to the exact pre-state and run the body the native code claims to replace.
  memcpy(c->ram, s.pre.data(), RAM_SIZE);
  memcpy(c->scratch, s.preSpad.data(), spad);
  *static_cast<R3000*>(c) = preRegs;
  body(c);
  s.subRam.assign(c->ram, c->ram + RAM_SIZE);
  s.subSpad.assign(c->scratch, c->scratch + spad);
  const R3000 subRegs = *static_cast<R3000*>(c);

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
      cfg_loge("ndiff", "  %s reg %s: native=0x%08X substrate=0x%08X", name, kReg[i],
               natRegs.r[i], subRegs.r[i]);
      diffs++;
    }
  if (natRegs.hi != subRegs.hi || natRegs.lo != subRegs.lo) {
    cfg_loge("ndiff", "  %s hi/lo: native=%08X/%08X substrate=%08X/%08X", name,
             natRegs.hi, natRegs.lo, subRegs.hi, subRegs.lo);
    diffs++;
  }

  // LEAVE THE NATIVE RESULT IN PLACE. Falling back to the substrate on a diff would make a broken
  // replacement behave correctly under the very flag meant to expose it.
  memcpy(c->ram, s.natRam.data(), RAM_SIZE);
  memcpy(c->scratch, s.natSpad.data(), spad);
  *static_cast<R3000*>(c) = natRegs;

  if (diffs) {
    site.diffs++;
    s.divergences++;
    cfg_loge("ndiff", "%s call #%d DIVERGES from the recompiled body (%d difference(s))",
             name, site.calls, diffs);
  } else {
    cfg_logi("ndiff", "%s call #%d matches the recompiled body exactly", name, site.calls);
  }
  return diffs != 0;
}

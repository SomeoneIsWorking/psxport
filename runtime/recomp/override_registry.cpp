// override_registry.cpp — implementation of the ONE native-override registry (see override_registry.h
// for the design: one entry per guest address carrying { native, gen }, one dispatch decision, shared
// by the two interception points — the g_<mod>_override[] thunk and rec_dispatch).
#include "override_registry.h"
#include "core.h"
#include "game.h"
#include "sbs.h"
#include "cfg.h"
#include "verify_harness.h"
#include "recomp_iface.h"   // seam: the generated per-module override setters (shard/ov_a00/ov_game)
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

inline uint32_t norm(uint32_t addr) { return (addr & 0x1FFFFFFFu) | 0x80000000u; }

struct Entry {
  uint32_t   addr;          // normalised (KSEG0) guest address
  const char* name;         // trace/ovhit label, or nullptr
  OverrideFn native;        // runs on the game core
  OverrideFn gen;           // runs on the oracle leg (== native for oracle-allowed primitives)
  uint64_t   nativeHits;    // core A (game) hit count
  uint64_t   oracleHits;    // core B (substrate) hit count
};

constexpr int kCap = 512;
Entry    g_tab[kCap];
int      g_n  = 0;
uint32_t g_lo = 0xFFFFFFFFu, g_hi = 0;   // [min,max] normalised addr — fast reject on the hot path

int lookup(uint32_t k) {
  if (k < g_lo || k > g_hi) return -1;
  for (int i = 0; i < g_n; i++) if (g_tab[i].addr == k) return i;
  return -1;
}

// PSXPORT_THUNK_FORCE_GEN=0xADDR[,0xADDR2,...] — force the listed addresses to their gen body even on
// the game core (treat them as oracle). Bisection knob: when a native cluster diverges under SBS,
// force-gen it to confirm it is the culprit (core A then matches core B). Parsed once, lazily.
const char* g_forceGen = (const char*)1;   // sentinel: parse on first use
bool forced(uint32_t addr) {
  if (g_forceGen == (const char*)1) {
    g_forceGen = getenv("PSXPORT_THUNK_FORCE_GEN");
    if (g_forceGen) cfg_logi("overrides", "FORCE_GEN active: %s", g_forceGen);
  }
  for (const char* p = g_forceGen; p && *p; ) {
    uint32_t a = (uint32_t)strtoul(p, (char**)&p, 0);
    if (norm(a) == addr) return true;
    while (*p == ',' || *p == ' ') p++;
  }
  return false;
}

void dump_atexit() {
  if (!cfg_dbg("ovhit") || g_n == 0) return;
  cfg_logi("ovhit", "override registry hit counts (native=coreA / oracle=coreB):");
  for (int i = 0; i < g_n; i++) {
    const Entry& e = g_tab[i];
    char label[32];
    const char* name = e.name;
    if (!name) { snprintf(label, sizeof label, "0x%08X", e.addr); name = label; }
    cfg_logi("override_registry", "  0x%08X %-34s : native=%llu  oracle=%llu%s%s", e.addr, name, (unsigned long long)e.nativeHits, (unsigned long long)e.oracleHits,
            e.nativeHits == 0 && e.oracleHits == 0 ? "   <-- NEVER HIT (registered but unreached)" : "",
            (e.oracleHits != 0 && e.nativeHits != e.oracleHits) ? "   <-- COUNT MISMATCH (control-flow divergence)" : "");
  }
}

// The ONE dispatch decision, shared by the thunk and rec_dispatch. `slot` is a valid g_tab index.
void runEntry(Core* c, int slot) {
  Entry& e = g_tab[slot];

  const bool oracle = (c->game && (c->game->psx_fallback || c->game->verify.inSubstrateLeg))
                      || forced(e.addr);
  if (oracle) { e.oracleHits++; e.gen(c); return; }

  e.nativeHits++;
  if (cfg_dbg("dispatch")) {
    Sbs* sbs = c->game ? c->game->sbs : nullptr;
    int cid = sbs ? sbs->coreId(c) : -1;
    cfg_logf("dispatch", "f%u core=%c 0x%08X %s ra=%08X a0=%08X a1=%08X a2=%08X a3=%08X",
             sbs ? sbs->frame() : 0, cid < 0 ? '-' : (cid ? 'B' : 'A'), e.addr,
             e.name ? e.name : "?", c->r[31], c->r[4], c->r[5], c->r[6], c->r[7]);
  }
  // PSXPORT_MIRROR_VERIFY[=all|=0xADDR,...]: per-invocation mechanical oracle. strictCheck replays the
  // pure gen leg via rec_dispatch with inSubstrateLeg=true; the oracle test above honours that flag, so
  // nested wired calls during the replay also stay pure-gen all the way down.
  if (c->game && c->game->verify.mirrorSampleGate(e.addr)) {
    struct Ctx { OverrideFn fn; Core* c; } ctx{ e.native, c };
    auto go = [](void* p) { Ctx* x = (Ctx*)p; x->fn(x->c); };
    c->game->verify.strictCheck(e.addr, go, &ctx);
  } else {
    e.native(c);
  }
}

// The shared thunk installed into every registered g_<mod>_override[] slot. The wrapper stamps c->pc to
// the guest address immediately before invoking it, so c->pc is exactly the entry address here.
void thunk(Core* c) {
  int slot = lookup(norm(c->pc));
  if (slot < 0) {
    cfg_logi("overrides", "thunk with no entry for pc=%08X (table has %d)", c->pc, g_n);
    abort();
  }
  runEntry(c, slot);
}

}  // namespace

namespace overrides {

void install(uint32_t addr, const char* name, OverrideFn native, OverrideFn gen, Setter setter) {
  const uint32_t k = norm(addr);
  int slot = lookup(k);
  if (slot < 0) {
    if (g_n >= kCap) { cfg_logi("overrides", "registry full (kCap=%d)", kCap); abort(); }
    slot = g_n++;
    g_tab[slot].nativeHits = g_tab[slot].oracleHits = 0;
    if (k < g_lo) g_lo = k;
    if (k > g_hi) g_hi = k;
    static bool s_atexit = false;
    if (!s_atexit) { s_atexit = true; atexit(dump_atexit); }
  }
  g_tab[slot].addr = k;
  g_tab[slot].name = name;
  g_tab[slot].native = native;
  g_tab[slot].gen = gen;

  // setter == nullptr: rec_dispatch interception only (no direct-call thunk) — see the header.
  if (setter) setter(addr, thunk);   // install the shared thunk into the module's g_<mod>_override[] slot
}

bool dispatch(Core* c, uint32_t addr) {
  int slot = lookup(norm(addr));
  if (slot < 0) return false;
  runEntry(c, slot);
  return true;
}

}  // namespace overrides

// These thin forwarders pick the recompiler module whose g_<mod>_override[] table the shared thunk is
// installed into. The module setters are generated symbols reached through the RecompRegistry seam
// (recomp_iface.h), so the framework itself names no generated symbol. `overrides::Setter` and the
// registry's RecOverrideFn are both void(*)(uint32_t, void(*)(Core*)) — the same type — so the seam's
// setter pointer is passed straight through.
void engine_set_override_main(uint32_t addr, OverrideFn native, OverrideFn gen) {
  overrides::install(addr, nullptr, native, gen, psxport_recomp()->shard_set_override);
}
void engine_set_override_a00(uint32_t addr, OverrideFn native, OverrideFn gen) {
  overrides::install(addr, nullptr, native, gen, psxport_recomp()->ov_a00_set_override);
}
void engine_set_override_game(uint32_t addr, OverrideFn native, OverrideFn gen) {
  overrides::install(addr, nullptr, native, gen, psxport_recomp()->ov_game_set_override);
}

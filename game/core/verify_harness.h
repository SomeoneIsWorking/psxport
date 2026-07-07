// game/core/verify_harness.h — class VerifyHarness: the shared A/B verify scaffold, one instance per Game.
//
// Every `<chan>verify` diagnostic follows the same shape: snapshot RAM+scratchpad+regs, run the native
// body, snapshot again, roll back, super-call the recomp oracle, diff (excluding the dead stack window),
// and rate-limit the log through match/mismatch counters. The per-site scratch used to be copy-pasted
// static locals (two 2 MB malloc'd RAM buffers + `static long ng, nb` + a `static int s_v` cfg cache) —
// hidden globals shared across the two SBS cores. This class owns that state per-Game:
//   * ram0()/ramN() — the 2 MB main-RAM snapshot pair, malloc'd on first traced run.
//   * check(name)   — per-check match/mismatch counters, keyed by the gate/channel name.
//   * on(chan)      — lazy cfg_dbg cache per channel (latches the value at first use, like the old
//                     `static int s_v = -1` sites — a later REPL `debug <chan>` before first use still wins).
//   * run(...)      — the common fn(Core*)-shaped gate (was class VerifyGate in game/world).
// Gates are DORMANT diagnostic channels (REPL `debug <chan>`); off by default. Reached as
// `c->game->verify.<...>`. Back-pointer `core` wired in Game's constructor.
#ifndef GAME_CORE_VERIFY_HARNESS_H
#define GAME_CORE_VERIFY_HARNESS_H
#include <cstdint>
struct Core;
void rec_super_call(Core*, uint32_t);

class VerifyHarness {
public:
  Core* core = nullptr;

  // Per-check counters + the lazily-latched cfg flag, keyed by the gate/channel name.
  struct Check {
    const char* name = nullptr;
    long nMatch = 0;      // was the per-site `static long ng`
    long nMismatch = 0;   // was the per-site `static long nb`
    int  flag = -1;       // was the per-site `static int s_v` cfg_dbg cache (-1 = not latched yet)
  };

  // Lazy cfg_dbg(chan) cache: latches on first call (same semantics as the old static-local caches).
  int on(const char* chan);

  // The counters entry for `name` (created on first use; `name` must be a literal that stays alive).
  Check& check(const char* name);

  // 2 MB main-RAM snapshot pair, malloc'd on first use.
  uint8_t* ram0();
  uint8_t* ramN();

  // The common gate shape: run native `fn` (result in c->r[2]); when `on`, snapshot/rollback and
  // super-call the recomp body at `superAddr`, then diff RAM (minus the dead stack window) +
  // scratchpad + v0. (Was VerifyGate::run in game/world/verify_gate.cpp.)
  void run(uint32_t (*fn)(Core*), uint32_t superAddr, const char* gate, int on);

  // ---- STRICT mirror TDD gate (USER 2026-07-08: verification must be RUN, not asserted) ----------
  // MV_CHECK at a pc_faithful fork site: when armed for `addr` (PSXPORT_MIRROR_VERIFY=all or
  // =0xADDR[,..]), snapshot guest state, run the NATIVE mirror, rewind, replay the PURE substrate
  // body (EngineOverrides suppressed — exactly SBS core B), byte-compare RAM + scratchpad + the
  // ABI register set (v0/v1, s0-s7, gp/sp/fp/ra, hi/lo) with NO exemptions (no dead-stack window),
  // and ABORT on any diff. On a match, execution continues from the native result.
  // Limits: yield-free mirrors only (scheduler_yield aborts while inCheck); host hw side effects
  // run twice while armed (do not arm CD-advancing leaves — SBS covers those); no nesting.
  bool strictArmed(uint32_t addr);      // also false while inCheck (no nesting)
  void strictCheck(uint32_t addr, void (*fn)(void*), void* ctx);
  bool inSubstrateLeg = false;          // rec_dispatch: suppress EngineOverrides (pure-B leg)
  bool inCheck = false;                 // no-nesting + scheduler_yield guard

private:
  static constexpr int kMaxChecks = 40;
  Check mChecks[kMaxChecks];
  int   mNChecks = 0;
  uint8_t* mRam0 = nullptr;
  uint8_t* mRamN = nullptr;
  // strict-gate state
  int      mStrictMode = -1;            // -1 unparsed, 0 off, 1 list, 2 all
  uint32_t mStrictList[32]; int mStrictN = 0;
  uint8_t* mStrictPreRam = nullptr; uint8_t* mStrictNatRam = nullptr;
  uint8_t  mStrictNatSpad[0x400];
  uint32_t mStrictNatRegs[16];
};

// Fork-site wrapper: strict-verify when armed, plain call otherwise.
#define MV_CHECK(c, addr, call)                                                            \
  do {                                                                                     \
    Core* mv_c = (c);                                                                      \
    if (mv_c->game && mv_c->game->verify.strictArmed(addr)) {                              \
      auto mv_fn = [&]() { call; };                                                        \
      struct MvRun { static void go(void* p) { (*static_cast<decltype(mv_fn)*>(p))(); } }; \
      mv_c->game->verify.strictCheck((addr), &MvRun::go, &mv_fn);                          \
    } else { call; }                                                                       \
  } while (0)

#endif

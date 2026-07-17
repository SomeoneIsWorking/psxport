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
#include <cstddef>
#include <vector>
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
  // body (the override registry's native dispatch suppressed — exactly SBS core B), byte-compare
  // RAM + scratchpad + the ABI register set (v0/v1, s0-s7, gp/sp/fp/ra, hi/lo) with NO exemptions
  // (no dead-stack window), and ABORT on any diff. On a match, execution continues from the native result.
  // Limits: yield-free mirrors only (scheduler_yield aborts while inCheck); host hw side effects
  // run twice while armed (do not arm CD-advancing leaves — SBS covers those); no nesting.
  bool strictArmed(uint32_t addr);      // also false while inCheck (no nesting)
  void strictCheck(uint32_t addr, void (*fn)(void*), void* ctx);

  // ---- write-journal fast path for strictCheck (2026-07-10) --------------------------------------
  // strictCheck used to snapshot+compare the FULL 2 MB main-RAM buffer twice per invocation (pre-leg
  // save, post-leg1 save, 2 MB linear compare) — with hot render-dispatch addresses firing 1000s of
  // times/frame this made `MIRROR_VERIFY=all EVERY=1` unable to finish even a few hundred frames in a
  // normal gate window. Root cause: comparing the WHOLE buffer when a typical leaf touches a few dozen
  // to a few hundred bytes. Fix: copy-on-first-write journaling. Every guest store to MAIN RAM (not
  // scratchpad — that stays a full 1 KB compare, already cheap) funnels through Core::mem_w8/16/32
  // (runtime/recomp/mem.cpp; audited as the SOLE write path during gameplay — game/*.cpp only ever
  // READS `c->ram[]` directly, and the two raw-write sites (boot.cpp/native_stub.cpp EXE-load memcpy)
  // run before any mirror check is ever armed). journalTrack(addr, orig) is called once per BYTE on
  // its first touch during an armed invocation (a dirty bitmap gates re-recording so a byte written
  // 1000 times is journaled once); strictCheck then snapshots/rewinds/compares only the UNION of both
  // legs' touched addresses instead of all 2 MB. See strictCheck's comment for the two-leg protocol.
  // PSXPORT_MIRROR_VERIFY_FULL=1 keeps the OLD full-2MB-scan path available (strictCheckFull) as the
  // authoritative slow reference the fast path is validated against — same verdicts, same first
  // mismatch, just slower. `journalTrack` is a no-op unless `mJournalArmed` (a single bool the hot
  // mem_w* path branches on — zero overhead when MIRROR_VERIFY is unset).
  bool journalIsArmed() const { return mJournalArmed; }
  void journalTrack(uint32_t ramOff, uint8_t origByte);

  // ---- Generalized "verify ALL wired overrides" gate (2026-07-10) --------------------------------
  // The CENTRAL native-dispatch injection point — the override registry's shared thunk (both the
  // `g_<mod>_override[]` direct-call path and `rec_dispatch`/`overrides::dispatch`, runtime/recomp/
  // override_registry.h) — calls mirrorSampleGate(addr) instead of strictArmed(addr) directly before
  // invoking the native handler. This makes `PSXPORT_MIRROR_VERIFY=all` cover EVERY wired address
  // automatically, with no per-call-site MV_CHECK needed: strictArmed's
  // mode/list parsing + inCheck no-nesting guard apply unchanged, PLUS per-address sampling
  // (PSXPORT_MIRROR_VERIFY_EVERY=N, default 1 = check every invocation — correctness over speed by
  // default; raise N to trade coverage for FPS on a hot address). A call site that already wraps
  // itself in MV_CHECK (e.g. sequencer.cpp's nat_frameTick) is unaffected: the outer strictCheck
  // sets inCheck=true before invoking the native leg, so the inner MV_CHECK's own strictArmed(addr)
  // sees inCheck and falls through to a plain call — no double-check, no double cost.
  bool mirrorSampleGate(uint32_t addr);

  // ---- fork-level SKIP-vs-FAITHFUL observable TDD gate (USER 2026-07-08) -------------------------
  // SV_CHECK at a pc_skip fork: when armed (PSXPORT_SKIP_VERIFY=all or =0xADDR[,..]), run the SKIP
  // leg, snapshot the OBSERVABLE set (game/core/observables.h positive list + 512 KB SPU RAM),
  // rewind guest+SPU state, run the ORACLE leg — the substrate arc the skip leg replaces, inline
  // and synchronous (in_stage forced 0 so yield/selfClose prims are no-ops; the override registry's
  // native dispatch suppressed) — then compare the observables and ABORT on any diff. Execution continues from
  // the SKIP result. Everything OUTSIDE the observable list (stack scratch, cadence counters) is
  // legitimately different between the paths and is NOT compared.
  bool skipArmed(uint32_t addr);
  void skipCheck(uint32_t addr, void (*skipFn)(void*), void* skipCtx,
                 void (*oracleFn)(void*), void* oracleCtx);
  bool inSubstrateLeg = false;          // rec_dispatch: suppress the override registry's native dispatch (pure-B leg)
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
  uint8_t* mStrictPreRam = nullptr; uint8_t* mStrictNatRam = nullptr;   // strictCheckFull only
  uint8_t  mStrictNatSpad[0x400];
  uint32_t mStrictNatRegs[16];
  int      mFullMode = -1;              // -1 unparsed; PSXPORT_MIRROR_VERIFY_FULL (forces strictCheckFull)
  void strictCheckFull(uint32_t addr, void (*fn)(void*), void* ctx);   // old full-2MB-scan path (validation reference)
  // journal fast-path state (see header comment above)
  struct JEntry { uint32_t addr; uint8_t orig; uint8_t nat; };
  bool     mJournalArmed = false;
  uint8_t* mJournalDirty = nullptr;     // 1 bit / RAM byte (0x200000/8 = 256 KiB), lazily calloc'd
  std::vector<JEntry> mJournal;         // touched-byte log for the in-flight strictCheck invocation
  // mirrorSampleGate: per-address invocation counter + PSXPORT_MIRROR_VERIFY_EVERY sampling.
  static constexpr int kMaxMirrorAddrs = 256;
  int      mMirrorEvery = -1;           // -1 unparsed
  uint32_t mMirrorCntAddr[kMaxMirrorAddrs] = {0};
  uint64_t mMirrorCnt[kMaxMirrorAddrs] = {0};
  int      mMirrorCntN = 0;
  uint64_t mLastMirrorCount = 0;        // invocation # of the check strictCheck is about to run
  // skip-gate state
  int      mSkipMode = -1;
  uint32_t mSkipList[32]; int mSkipN = 0;
  uint8_t* mSkipSpuA = nullptr; uint8_t* mSkipSpuB = nullptr; uint8_t* mSkipSpuPre = nullptr;
};

// Fork-site wrapper for the SKIP-vs-FAITHFUL observable gate: `skip_call` is the pc_skip leg,
// `oracle_call` sets up regs and dispatches the substrate arc the skip leg replaces (it runs
// with yield prims no-op'd, so use the TASK BODY address for spawn-and-wait arcs, e.g.
// rec_dispatch(c, 0x800452C0) — NOT the 0x80044BD4 wrapper, whose wait loop can never complete
// inline).
#define SV_CHECK(c, addr, skip_call, oracle_call)                                              \
  do {                                                                                         \
    Core* sv_c = (c);                                                                          \
    if (sv_c->game && sv_c->game->verify.skipArmed(addr)) {                                    \
      auto sv_s = [&]() { skip_call; };                                                        \
      auto sv_o = [&]() { oracle_call; };                                                      \
      struct SvS { static void go(void* p) { (*static_cast<decltype(sv_s)*>(p))(); } };        \
      struct SvO { static void go(void* p) { (*static_cast<decltype(sv_o)*>(p))(); } };        \
      sv_c->game->verify.skipCheck((addr), &SvS::go, &sv_s, &SvO::go, &sv_o);                  \
    } else { skip_call; }                                                                      \
  } while (0)

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

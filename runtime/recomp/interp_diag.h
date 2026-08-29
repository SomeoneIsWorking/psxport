// interp_diag.h — InterpDiag: the flat interpreter's trace/profile/diagnostic state (interp.cpp).
// One per Core (`c->idiag`) so two SBS cores never interleave call traces, profiles, or hazard
// detectors through shared buffers. All fields are pure diagnostics — none affect guest state.
#pragma once
#include <cstdint>
#include <cstdio>
#include <map>

struct InterpDiag {
  // Optional call trace (PSXPORT_INTERP_TRACE=<path> / REPL `trace`): jal/jalr targets.
  FILE *trace_fp = nullptr;

  // Differential NATIVE-CALL tracer (PSXPORT_NCALL_TRACE=<path>) — see interp.cpp for the diff recipe.
  FILE *ncall_fp = nullptr;
  long ncall_seq = 0;
  int ncall_init = 0;

  // Load-delay hazard detector (`debug ldhazard`).
  int ldhaz = -1;
  long ldhaz_n = 0;
  uint32_t ld_last_in = 0, ld_last_pc = 0; // last instruction in EXECUTION order

  // Interpreted-function tripwire (PSXPORT_INTERP_FUNCS=<path>).
  FILE *ifn_fp = nullptr;
  int ifn_init = 0;
  uint32_t ifn_set[1 << 14] = {}; // 16384 slots; addrs are non-zero so 0 == empty
  int ifn_count = 0;

  // Interpreter perf profiler (REPL `prof start` / `prof dump <path>`).
  int prof_on = 0;                       // toggled by REPL `prof start`/`prof off`
  uint64_t prof_pc[1 << 17] = {};        // 131072 buckets, 16 bytes each (aligns to fn starts)
  uint64_t prof_total = 0;               // total instructions counted
  uint32_t prof_call_addr[1 << 14] = {}; // call-target set (0 == empty)
  uint64_t prof_call_n[1 << 14] = {};    // parallel call counts
  uint64_t prof_call_total = 0;          // total interpreted-fn entries counted

  // Derail diagnostics: ring of last compiled-function entries.
  uint32_t callring[64] = {};
  int callring_pos = 0;

  // Substrate bisect gate cache (PSXPORT_SUBSTRATE_LO/HI, hex KSEG0 addrs).
  int sg_init = 0;
  uint32_t sg_lo = 0, sg_hi = 0;

  // Spin detector cache (`debug spin`).
  int spindbg = -1;

  // PSXPORT_PCTRAP=0xADDR guest-call-chain dump (with SKIP count + hit counter).
  uint32_t pctrap = 0xFFFFFFFFu; // 0xFFFFFFFF = env not read yet; 0 = off
  long pctrap_skip = 0;
  long pctrap_hit = 0;

  // Dispatch-decision ring (issue crashbash-0018 discriminator). The flaky first-boot recomp-MISS at
  // 0x80012840 prints an addr whose main_dispatch case EXISTS, so the canned miss diagnostic provably
  // misreports the mechanism — but the miss only fires in plain runs and never under channel
  // instrumentation, so a printf-at-decision-time probe changes the timing it is meant to observe.
  // This ring records EVERY rec_dispatch entry and the branch it took (ENTER/MAIN/LIVE/FIXED/AMBIG/
  // OVERRIDE/MISSDROP), plus rec_dispatch_miss entries, into a fixed per-core buffer with NO I/O —
  // the same shape as a plain run. When the fatal miss fires, hle.cpp dumps the ring in order, and
  // the last decision before the MISS marker names the branch (or the bypass) that produced it.
  // Ring order: index walks oldest->newest; dispdec_pos is the NEXT write slot.
  struct DispDecision {
    uint32_t addr; // dispatch target as rec_dispatch received it (KSEG0)
    uint32_t ra;   // c->r[31] at entry
    uint32_t kind; // DISPDEC_* below
    uint32_t aux;  // branch detail: overlay index for LIVE/FIXED, else 0
  };
  static constexpr uint32_t DISPDEC_ENTER = 1, DISPDEC_MAIN = 2, DISPDEC_LIVE = 3, DISPDEC_FIXED = 4, DISPDEC_AMBIG = 5,
                            DISPDEC_OVERRIDE = 6, DISPDEC_MISSDROP = 7, DISPDEC_MISS = 8;
  static constexpr int DISPDEC_CAP = 128;
  DispDecision dispdec[DISPDEC_CAP] = {};
  int dispdec_pos = 0; // next write slot; == dispdec_n while not yet wrapped
  int dispdec_n = 0;   // entries recorded (saturates at DISPDEC_CAP once wrapped)
  void dispdecRecord(uint32_t addr, uint32_t ra, uint32_t kind, uint32_t aux = 0) {
    dispdec[dispdec_pos] = {addr, ra, kind, aux};
    dispdec_pos = (dispdec_pos + 1) % DISPDEC_CAP;
    if (dispdec_n < DISPDEC_CAP) {
      dispdec_n++;
    }
  }
  // Oldest-first dump on the "dispdec" channel; implemented out-of-line in interp_diagnostics.cpp
  // so a unit test can drive record+dump without linking the dispatch/router substrate.
  void dumpDispdec() const;

  // rec_dispatch diagnostics (overlay_router.cpp):
  // `debug recdep` — histogram of substrate dispatch targets (top-40 dumped at exit).
  std::map<uint32_t, uint64_t> recdep;
  // PSXPORT_DISPWATCH=0xADDR[:ra=0xRA] cache.
  uint32_t dispwatch = 0xFFFFFFFFu; // 0xFFFFFFFF = env not read yet; 0 = off
  uint32_t dispwatch_ra = 0;

  // OT/GTE submission-attribution shadow stack — "who is this GP0/GTE submission attributed to".
  //
  // MAINTAINED ON EVERY GUEST CALL, both kinds, since 2026-08-12: the recompiler emits the push/pop in
  // each guest function's WRAPPER (tools/recomp/emit.py), and both direct calls and the generated
  // dispatch switches go through that wrapper. It is NOT channel-gated, so the attribution has the same
  // shape in a normal run as in a diagnostic one.
  //
  // THE INVARIANT USED TO BE THE OPPOSITE and it is worth knowing why it changed, because the old shape
  // produced a measured failure rather than merely being incomplete: pushes happened only around INDIRECT
  // (jalr) dispatch, while the packet-pool stores are performed by shared SDK-adjacent routines reached by
  // direct jal. Those stores therefore always read an EMPTY stack, and the graphics-producer DB's guest
  // leg attributed 1.61% of its prims. Maintaining it on direct calls took that to 98.47%, priced at
  // +0.0% user CPU over a 1200-frame replay (see emit.py's note for the measurement).
  //
  // This is a call-attribution stack, not an instruction-level trace. Depth may exceed the cap; only the
  // top OTATTR_CAP frames are KEPT, while `otattr_depth` keeps counting, so push/pop never desyncs and
  // otattrTop() returns an honest 0 above the cap rather than a stale frame.
  //
  // CAP RAISED 64 -> 256 with the invariant change, and not arbitrarily: the old stack only ever held
  // indirectly-dispatched frames, so it was shallow by construction; it now holds the real guest call
  // depth. Overflow does not corrupt anything, it silently stops attributing — which is a candidate for
  // the residual 1.36% of stores that still read no fn (unmeasured). 256 words is 1 KB per Core.
  static constexpr int OTATTR_CAP = 256;
  uint32_t otattr_stack[OTATTR_CAP] = {};
  int otattr_depth = 0;
  void otattrPush(uint32_t addr) {
    if (otattr_depth < OTATTR_CAP) {
      otattr_stack[otattr_depth] = addr;
    }
    otattr_depth++;
  }
  void otattrPop() {
    if (otattr_depth > 0) {
      otattr_depth--;
    }
  }
  uint32_t otattrTop() const {
    int d = otattr_depth;
    return (d > 0 && d <= OTATTR_CAP) ? otattr_stack[d - 1] : 0;
  }
  uint32_t otattrCaller() const {
    int d = otattr_depth;
    return (d > 1 && d - 1 <= OTATTR_CAP) ? otattr_stack[d - 2] : 0;
  }
  // The whole live chain, innermost first, for the `otchain` diagnostic and for CLAIM RESOLUTION (the
  // frame-selection policy: which frame in the chain a producer row claims). `i == 0` is the top.
  // Returns 0 past the end AND past the cap — a frame that overflowed the cap is not knowable here, and
  // reporting 0 rather than a stale word is what keeps "no claimed frame found" an honest answer.
  // ABOVE THE CAP THE CHAIN IS NOT KNOWABLE AND THIS SAYS SO, rather than returning a plausible frame.
  // otattrPush stores only while depth < CAP, so once the guest goes deeper the array holds the OUTERMOST
  // CAP frames and index CAP-1 is frame #CAP counting from the BOTTOM — not the top. Indexing from the
  // end there would hand back a frame from an unrelated part of the call tree that looks perfectly valid.
  // So visible depth is 0 when overflowed: callers get "I can see nothing", which is the truth.
  int otattrVisibleDepth() const {
    int d = otattr_depth;
    return d <= OTATTR_CAP ? d : 0;
  }
  uint32_t otattrFrameFromTop(int i) const {
    const int d = otattrVisibleDepth();
    return (i >= 0 && i < d) ? otattr_stack[d - 1 - i] : 0;
  }
};

// interp_diag.h — InterpDiag: the flat interpreter's trace/profile/diagnostic state (interp.cpp).
// One per Core (`c->idiag`) so two SBS cores never interleave call traces, profiles, or hazard
// detectors through shared buffers. All fields are pure diagnostics — none affect guest state.
#pragma once
#include <cstdint>
#include <cstdio>
#include <map>

struct InterpDiag {
  // Optional call trace (PSXPORT_INTERP_TRACE=<path> / REPL `trace`): jal/jalr targets.
  FILE* trace_fp = nullptr;

  // Differential NATIVE-CALL tracer (PSXPORT_NCALL_TRACE=<path>) — see interp.cpp for the diff recipe.
  FILE* ncall_fp = nullptr;
  long  ncall_seq = 0;
  int   ncall_init = 0;

  // Load-delay hazard detector (`debug ldhazard`).
  int      ldhaz = -1;
  long     ldhaz_n = 0;
  uint32_t ld_last_in = 0, ld_last_pc = 0;   // last instruction in EXECUTION order

  // Interpreted-function tripwire (PSXPORT_INTERP_FUNCS=<path>).
  FILE*    ifn_fp = nullptr;
  int      ifn_init = 0;
  uint32_t ifn_set[1 << 14] = {};            // 16384 slots; addrs are non-zero so 0 == empty
  int      ifn_count = 0;

  // Interpreter perf profiler (REPL `prof start` / `prof dump <path>`).
  int      prof_on = 0;                      // toggled by REPL `prof start`/`prof off`
  uint64_t prof_pc[1 << 17] = {};            // 131072 buckets, 16 bytes each (aligns to fn starts)
  uint64_t prof_total = 0;                   // total instructions counted
  uint32_t prof_call_addr[1 << 14] = {};     // call-target set (0 == empty)
  uint64_t prof_call_n[1 << 14] = {};        // parallel call counts
  uint64_t prof_call_total = 0;              // total interpreted-fn entries counted

  // Derail diagnostics: ring of last compiled-function entries.
  uint32_t callring[64] = {};
  int      callring_pos = 0;

  // Substrate bisect gate cache (PSXPORT_SUBSTRATE_LO/HI, hex KSEG0 addrs).
  int      sg_init = 0;
  uint32_t sg_lo = 0, sg_hi = 0;

  // Spin detector cache (`debug spin`).
  int spindbg = -1;

  // PSXPORT_PCTRAP=0xADDR guest-call-chain dump (with SKIP count + hit counter).
  uint32_t pctrap = 0xFFFFFFFFu;             // 0xFFFFFFFF = env not read yet; 0 = off
  long     pctrap_skip = 0;
  long     pctrap_hit = 0;

  // rec_dispatch diagnostics (overlay_router.cpp):
  // `debug recdep` — histogram of substrate dispatch targets (top-40 dumped at exit).
  std::map<uint32_t, uint64_t> recdep;
  // PSXPORT_DISPWATCH=0xADDR[:ra=0xRA] cache.
  uint32_t dispwatch = 0xFFFFFFFFu;          // 0xFFFFFFFF = env not read yet; 0 = off
  uint32_t dispwatch_ra = 0;
};

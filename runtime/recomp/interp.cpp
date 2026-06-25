// The runtime's ONE execution substrate: a flat Core interpreter that runs all guest code
// (the boot stub, MAIN.EXE, and the disc code overlays) directly from g_ram. The static
// recompiler was dropped from the build (see docs/journal.md "later-101"); emit.py survives
// only as an offline analysis aid. There is a single instruction core (exec_simple) and a
// single control-flow loop (interp_flat); the two public entry points — rec_coro_run (a
// cooperative task) and rec_interp (a synchronous nested call / super-call) — are thin
// wrappers over it that differ only in their return sentinel. See the wrappers at the bottom.
//
// The loop is FLAT (not C-recursive): jal/jalr set the link reg and JUMP; the called code keeps
// its own frame on the PSX stack in g_ram and returns by `jr ra`. This lets a cooperative yield
// longjmp out and resume mid-chain (native_boot.c), and means PSX call depth costs no C stack.
// Native overrides and BIOS vectors are invoked as C (coro_native_call); everything else runs
// interpreted. Instruction semantics match the emitter (tools/recomp/emit.py) bit-for-bit, so
// the offline-recompiled C and the interpreter agree.
//
// Faithful-first simplifications match the emitter: no load-delay slot; add==addu; signed
// div/mult via cpu_div/mult helpers; GTE via gte_op/gte_read/write.
#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>

void rec_dispatch(Core* c, uint32_t addr);
void rec_syscall(Core* c, uint32_t code);
void rec_break(Core* c, uint32_t code);
void rec_interp(Core* c, uint32_t pc);

// Diagnostics: the PC currently being interpreted (read by the watchdog on a stall to report
// WHERE the interpreter is spinning). Optional call trace (PSXPORT_INTERP_TRACE=<path>) logs
// jal/jalr targets — used to follow the boot stub and find where it wedges.
volatile uint32_t g_interp_pc = 0;
// Entry address of the override currently being dispatched — set right before an override fn runs, so a
// bracket override (one fn registered at SEVERAL scanned overlay entries) can super-call the exact body
// it intercepted instead of a stale stored address. Read immediately on override entry.
uint32_t g_override_tgt = 0;
static FILE* g_trace_fp = 0;
void interp_trace_open(const char* path) {
  if (path && *path) { g_trace_fp = fopen(path, "w"); if (!g_trace_fp) perror(path);
    else setvbuf(g_trace_fp, 0, _IOLBF, 0); }
  else if (g_trace_fp) { fclose(g_trace_fp); g_trace_fp = 0; }   // empty path = close
}
static inline void trace_call(uint32_t from, uint32_t to) {
  if (g_trace_fp) fprintf(g_trace_fp, "%08X -> %08X\n", from, to);
}

// ---- Differential NATIVE-CALL tracer (PSXPORT_NCALL_TRACE=<path>) ---------------------------------
// Every native override / BIOS call the interpreter makes is logged with its inputs (a0-a3) and
// outputs (v0/v1). Since the interpreter + memory are deterministic and (after the OOP refactor)
// byte-identical, two builds produce the SAME native-call sequence until a native function behaves
// differently. Diffing two traces, the FIRST line with identical inputs but different outputs is the
// exact override whose conversion broke (a different return value / register effect); the first line
// with differing INPUTS means an earlier call's memory side-effect diverged. tools/ncall_diff.py
// runs both builds and reports that first divergence. Zero cost when the env var is unset.
static FILE* g_ncall_fp = 0;
static long  g_ncall_seq = 0;
static int   g_ncall_init = 0;
static void ncall_open_once(void) {
  if (g_ncall_init) return;
  g_ncall_init = 1;
  const char* p = cfg_str("PSXPORT_NCALL_TRACE");
  if (p && *p) { g_ncall_fp = fopen(p, "w"); if (!g_ncall_fp) perror(p);
                 else setvbuf(g_ncall_fp, 0, _IOLBF, 0); }
}
// kind: 'O' = address-keyed override, 'B' = BIOS vector. Logged AFTER the native fn runs.
static inline void ncall_log(char kind, uint32_t tgt, uint32_t a0, uint32_t a1, uint32_t a2,
                             uint32_t a3, uint32_t v0, uint32_t v1) {
  if (!g_ncall_fp) return;
  fprintf(g_ncall_fp, "%ld %c %08X  a:%08X %08X %08X %08X -> v:%08X %08X\n",
          g_ncall_seq++, kind, tgt, a0, a1, a2, a3, v0, v1);
}

#define RS(i)  (((i) >> 21) & 31)
#define RT(i)  (((i) >> 16) & 31)
#define RD(i)  (((i) >> 11) & 31)
#define SH(i)  (((i) >> 6) & 31)
#define FN(i)  ((i) & 63)
#define IMM(i) ((uint32_t)(uint16_t)(i))
#define SIMM(i)((uint32_t)(int32_t)(int16_t)(i))
#define TGT(i, pc) (((pc) & 0xF0000000u) | (((i) & 0x03FFFFFFu) << 2))

#define W(n, v) do { uint32_t _n = (n); if (_n) c->r[_n] = (v); } while (0)

// ---- Core load-delay hazard DETECTOR (PSXPORT_LDHAZARD) ------------------------------------
// Our interpreter omits the load-delay slot (interp.c head): a load's target reg is visible to
// the immediately-following instruction, whereas real Core (and the mednafen oracle) make that
// instruction read the OLD value. Compiler-scheduled code fills the slot (nop / independent op),
// so the hazard should be ABSENT. This detector counts cases where instruction I reads a GPR
// that instruction I-1 loaded into — i.e. where our no-delay model can diverge from hardware.
// Logs the first hits with PC so we can see whether the corrupt rendering path even has them.
// Returns the GPR a load writes (target), or 0 if `in` is not a GPR-target load.
static int ld_target(uint32_t in) {
  uint32_t op = in >> 26;
  if (op >= 0x20 && op <= 0x26) return RT(in);          // lb lh lwl lw lbu lhu lwr
  if (op == 0x10 && RS(in) == 0x00) return RT(in);      // mfc0
  if (op == 0x12 && (RS(in) == 0x00 || RS(in) == 0x02)) return RT(in); // mfc2 / cfc2
  return 0;
}
// Does `in` read GPR r as a source operand?
static int reads_gpr(uint32_t in, int r) {
  if (r == 0) return 0;
  uint32_t op = in >> 26, f = FN(in);
  switch (op) {
    case 0x00: // SPECIAL
      switch (f) {
        case 0x00: case 0x02: case 0x03: return RT(in) == r;            // sll srl sra (rt, sh imm)
        case 0x08: return RS(in) == r;                                  // jr
        case 0x09: return RS(in) == r;                                  // jalr
        case 0x10: case 0x12: return 0;                                 // mfhi mflo
        case 0x11: case 0x13: return RS(in) == r;                       // mthi mtlo
        default:   return RS(in) == r || RT(in) == r;                   // arith/logic/shiftv/mul/div
      }
    case 0x0F: return 0;                                                // lui
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E:
      return RS(in) == r;                                               // addi.. ori.. (rs)
    case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: return RS(in) == r;   // loads: base
    case 0x22: case 0x26: return RS(in) == r || RT(in) == r;            // lwl/lwr: base + merge
    case 0x28: case 0x29: case 0x2B: case 0x2A: case 0x2E:              // stores
      return RS(in) == r || RT(in) == r;
    case 0x04: case 0x05: return RS(in) == r || RT(in) == r;            // beq bne
    case 0x06: case 0x07: case 0x01: return RS(in) == r;                // blez bgtz regimm
    case 0x10: return RS(in) == 0x04 && RT(in) == r;                    // mtc0 (rt)
    case 0x12: return (RS(in) == 0x04 || RS(in) == 0x06) && RT(in) == r; // mtc2/ctc2 (rt)
    case 0x32: return RS(in) == r;                                      // lwc2 base
    case 0x3A: return RS(in) == r;                                      // swc2 base
    default: return 0;
  }
}
static int g_ldhaz = -1;
static long g_ldhaz_n = 0;
static uint32_t g_ld_last_in = 0, g_ld_last_pc = 0;   // last instruction in EXECUTION order
// Check the just-fetched instruction (`in`@`pc`, about to execute) against the previously
// executed one, then make it the new "last". Called in execution order — INCLUDING delay slots —
// so a load in a jump/branch delay slot is checked against the branch TARGET (the real next op).
static inline void ldhaz_step(uint32_t in, uint32_t pc) {
  if (g_ldhaz < 0) g_ldhaz = cfg_dbg("ldhazard") ? 1 : 0;
  if (g_ldhaz) {
    uint32_t p = g_ld_last_in; int t = ld_target(p);
    // Skip the lwl/lwr unaligned-merge idiom (same rt): our no-delay model merges correctly.
    int merge = ((p >> 26) == 0x22 && (in >> 26) == 0x26 && RT(p) == RT(in)) ||
                ((p >> 26) == 0x26 && (in >> 26) == 0x22 && RT(p) == RT(in));
    if (t && !merge && reads_gpr(in, t) && g_ldhaz_n++ < 60)
      fprintf(stderr, "[ldhaz] load r%d @%08X (%08X) -> read by next @%08X (%08X)\n",
              t, g_ld_last_pc, p, pc, in);
  }
  g_ld_last_in = in; g_ld_last_pc = pc;
}

// OVERRIDE TABLE REMOVED (2026-06-22) — top-down PC-driven model: PC calls PC directly; PSX never
// calls PC. The address-keyed override table (g_iov / iov_add / rec_set_interp_override[_auto] /
// interp_override_for / iov_flush_auto_range) and the overlay-load hook plumbing
// (rec_set_overlay_load_hook / rec_overlay_loaded) are GONE — they existed only to flip the
// interpreter into native code at a registered address. Native code is now invoked by PC calling
// the native function directly.

static int is_bios(uint32_t a) { uint32_t p = a & 0x1FFFFFFF; return p==0xA0||p==0xB0||p==0xC0; }

// Execute one non-control instruction (delay-slot-safe; no branches/jumps/loads-delay).
static void exec_simple(Core* c, uint32_t in) {
  uint32_t op = in >> 26;
  if (in == 0) return;  // nop
  switch (op) {
    case 0x00: {  // SPECIAL
      uint32_t f = FN(in), rs = RS(in), rt = RT(in), rd = RD(in), sh = SH(in);
      switch (f) {
        case 0x00: W(rd, c->r[rt] << sh); break;                              // sll
        case 0x02: W(rd, c->r[rt] >> sh); break;                             // srl
        case 0x03: W(rd, (uint32_t)((int32_t)c->r[rt] >> sh)); break;        // sra
        case 0x04: W(rd, c->r[rt] << (c->r[rs] & 31)); break;                // sllv
        case 0x06: W(rd, c->r[rt] >> (c->r[rs] & 31)); break;                // srlv
        case 0x07: W(rd, (uint32_t)((int32_t)c->r[rt] >> (c->r[rs] & 31))); break; // srav
        case 0x10: W(rd, c->hi); break;                                      // mfhi
        case 0x11: c->hi = c->r[rs]; break;                                  // mthi
        case 0x12: W(rd, c->lo); break;                                      // mflo
        case 0x13: c->lo = c->r[rs]; break;                                  // mtlo
        case 0x18: { int64_t p = (int64_t)(int32_t)c->r[rs] * (int64_t)(int32_t)c->r[rt];
                     c->lo = (uint32_t)p; c->hi = (uint32_t)((uint64_t)p >> 32); } break; // mult
        case 0x19: { uint64_t p = (uint64_t)c->r[rs] * (uint64_t)c->r[rt];
                     c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); } break;           // multu
        case 0x1A: cpu_div(c, c->r[rs], c->r[rt]); break;                    // div
        case 0x1B: cpu_divu(c, c->r[rs], c->r[rt]); break;                   // divu
        case 0x20: case 0x21: W(rd, c->r[rs] + c->r[rt]); break;             // add/addu
        case 0x22: case 0x23: W(rd, c->r[rs] - c->r[rt]); break;             // sub/subu
        case 0x24: W(rd, c->r[rs] & c->r[rt]); break;                        // and
        case 0x25: W(rd, c->r[rs] | c->r[rt]); break;                        // or
        case 0x26: W(rd, c->r[rs] ^ c->r[rt]); break;                        // xor
        case 0x27: W(rd, ~(c->r[rs] | c->r[rt])); break;                     // nor
        case 0x2A: W(rd, (uint32_t)((int32_t)c->r[rs] < (int32_t)c->r[rt])); break; // slt
        case 0x2B: W(rd, (uint32_t)(c->r[rs] < c->r[rt])); break;            // sltu
        case 0x0C: rec_syscall(c, (in >> 6) & 0xFFFFF); break;               // syscall
        case 0x0D: rec_break(c, (in >> 6) & 0xFFFFF); break;                 // break
        default: fprintf(stderr, "[interp] bad special funct 0x%02X\n", f); break;
      }
      break;
    }
    case 0x0F: W(RT(in), IMM(in) << 16); break;                             // lui
    case 0x08: case 0x09: W(RT(in), c->r[RS(in)] + SIMM(in)); break;        // addi/addiu
    case 0x0A: W(RT(in), (uint32_t)((int32_t)c->r[RS(in)] < (int32_t)SIMM(in))); break; // slti
    case 0x0B: W(RT(in), (uint32_t)(c->r[RS(in)] < SIMM(in))); break;       // sltiu
    case 0x0C: W(RT(in), c->r[RS(in)] & IMM(in)); break;                    // andi
    case 0x0D: W(RT(in), c->r[RS(in)] | IMM(in)); break;                    // ori
    case 0x0E: W(RT(in), c->r[RS(in)] ^ IMM(in)); break;                    // xori
    case 0x20: W(RT(in), (uint32_t)(int8_t)c->mem_r8(c->r[RS(in)] + SIMM(in))); break;   // lb
    case 0x24: W(RT(in), (uint32_t)c->mem_r8(c->r[RS(in)] + SIMM(in))); break;           // lbu
    case 0x21: W(RT(in), (uint32_t)(int16_t)c->mem_r16(c->r[RS(in)] + SIMM(in))); break; // lh
    case 0x25: W(RT(in), (uint32_t)c->mem_r16(c->r[RS(in)] + SIMM(in))); break;          // lhu
    case 0x23: W(RT(in), c->mem_r32(c->r[RS(in)] + SIMM(in))); break;                    // lw
    case 0x22: W(RT(in), c->mem_lwl(c->r[RT(in)], c->r[RS(in)] + SIMM(in))); break;      // lwl
    case 0x26: W(RT(in), c->mem_lwr(c->r[RT(in)], c->r[RS(in)] + SIMM(in))); break;      // lwr
    case 0x28: c->mem_w8(c->r[RS(in)] + SIMM(in), (uint8_t)c->r[RT(in)]); break;         // sb
    case 0x29: c->mem_w16(c->r[RS(in)] + SIMM(in), (uint16_t)c->r[RT(in)]); break;       // sh
    case 0x2B: c->mem_w32(c->r[RS(in)] + SIMM(in), c->r[RT(in)]); break;                 // sw
    case 0x2A: c->mem_swl(c->r[RS(in)] + SIMM(in), c->r[RT(in)]); break;                 // swl
    case 0x2E: c->mem_swr(c->r[RS(in)] + SIMM(in), c->r[RT(in)]); break;                 // swr
    case 0x10: {  // COP0
      uint32_t fmt = RS(in);
      if (fmt == 0x00) W(RT(in), cop0_mfc(c, RD(in)));        // mfc0
      else if (fmt == 0x04) cop0_mtc(c, RD(in), c->r[RT(in)]); // mtc0
      // rfe (in==0x42000010) is a no-op under HLE
      break;
    }
    case 0x12: {  // COP2 / GTE
      uint32_t fmt = RS(in);
      if (in & (1u << 25)) { gte_op(c, in); break; }          // GTE operation (cop2 bit25)
      if (fmt == 0x00) W(RT(in), gte_read_data(RD(in)));      // mfc2
      else if (fmt == 0x02) W(RT(in), gte_read_ctrl(RD(in))); // cfc2
      else if (fmt == 0x04) gte_write_data(RD(in), c->r[RT(in)]); // mtc2
      else if (fmt == 0x06) gte_write_ctrl(RD(in), c->r[RT(in)]); // ctc2
      break;
    }
    case 0x32: gte_write_data(RT(in), c->mem_r32(c->r[RS(in)] + SIMM(in))); break;       // lwc2
    case 0x3A: c->mem_w32(c->r[RS(in)] + SIMM(in), gte_read_data(RT(in))); break;        // swc2
    default:
      // FAIL-FAST (global rule, user 2026-06-22): a bad opcode means the PC derailed into data/garbage —
      // a real bug (wrong jump target / unloaded overlay / corrupt load). Do NOT limp on spewing thousands
      // of "bad opcode" lines; abort immediately with the derail site + a guest-stack backtrace so the
      // broken jump/load can be found and fixed at its root.
      fprintf(stderr, "\n[DERAIL] bad opcode 0x%02X insn=0x%08X at pc=0x%08X ra=0x%08X sp=0x%08X\n",
              op, in, g_interp_pc, c->r[31], c->r[29]);
      { uint32_t sp = c->r[29]; int shown = 0;
        for (uint32_t a = sp; a < sp + 512 && shown < 16; a += 4) {
          uint32_t w = c->mem_r32(a), k = w & 0x1FFFFFFF;
          if (k >= 0x10000 && k < 0x120000 && (w & 3) == 0) { fprintf(stderr, "   [sp+0x%03X]=0x%08X\n", a-sp, w); shown++; }
        } }
      // PSXPORT_DERAIL_DUMP=<path>: snapshot guest RAM at the derail so the offending overlay/jump can be
      // reverse-engineered (the overlay code isn't in static MAIN.EXE; disas.py --ram needs this dump).
      { const char* dp = cfg_str("PSXPORT_DERAIL_DUMP");
        if (dp) { FILE* df = fopen(dp, "wb");
          if (df) { fwrite(c->ram, 1, 0x200000, df); fclose(df);
                    fprintf(stderr, "[DERAIL] guest RAM dumped -> %s (2MB)\n", dp); } } }
      fflush(stderr); abort();
  }
}

// ---- The single flat interpreter loop ----------------------------------------------------
// Runs guest code from `pc` WITHOUT using the host C stack for PSX calls (so a yield can longjmp
// out and a later call resume mid-chain — see native_boot.c). jal/jalr set the link reg and
// JUMP; the called code keeps its own frame on the PSX stack in g_ram and returns by `jr ra`.
// The loop exits when a `jr ra` targets `stop_ra`: CORO_SENTINEL (0xDEAD0000) for a top-level
// task (set by the scheduler in native_boot.c), or the same value pushed by rec_interp for a
// synchronous nested call so it returns to its C caller. Native overrides (yield, CD, submit,
// ...) and BIOS vectors are invoked as C (an override may longjmp out, e.g. the yield);
// everything else is interpreted flat. This is the ucontext-free model: all a yield must
// save/restore is the PSX state (PC via the link/stack, regs, SP-in-g_ram).
#define CORO_SENTINEL 0xDEAD0000u
void rec_dispatch_miss(Core* c, uint32_t addr);

// ---- Interpreter burn-down tripwire (PSXPORT_INTERP_FUNCS=<path>) --------------------------------
// The top-down native-port metric: record every UNIQUE guest function the interpreter actually runs
// (i.e. a call target with NO native override / not a BIOS vector). When this set is empty across a
// boot->first-cutscene run, that path is 100% native (zero interpreter). Each newly-seen function is
// appended as `TO  <-from  count(so far=1)` so the file doubles as a representative call-edge map for
// building the port tree top-down. Open-addressing set, line-buffered so a kill/timeout still yields
// the list. Zero cost when the env var is unset.
static FILE*    g_ifn_fp = 0;
static int      g_ifn_init = 0;
static uint32_t g_ifn_set[1 << 14];               // 16384 slots; addrs are non-zero so 0 == empty
static int      g_ifn_count = 0;
static void ifn_open_once(void) {
  g_ifn_init = 1;
  const char* p = cfg_str("PSXPORT_INTERP_FUNCS");
  if (p && *p) { g_ifn_fp = fopen(p, "w"); if (!g_ifn_fp) perror(p);
                 else setvbuf(g_ifn_fp, 0, _IOLBF, 0); }
}
static inline void ifn_record(uint32_t tgt, uint32_t from) {
  if (!g_ifn_init) ifn_open_once();
  if (!g_ifn_fp) return;
  uint32_t h = (tgt * 2654435761u) >> 18, m = (1u << 14) - 1u;
  for (uint32_t i = 0; i < (1u << 14); i++) {
    uint32_t s = (h + i) & m;
    if (g_ifn_set[s] == tgt) return;              // already recorded
    if (g_ifn_set[s] == 0) { g_ifn_set[s] = tgt; g_ifn_count++;
      fprintf(g_ifn_fp, "%08X  <-%08X  [#%d]\n", tgt, from, g_ifn_count); return; }
  }
}

// ---- Interpreter PERF PROFILER (REPL `prof start` / `prof dump <path>`) --------------------------
// The perf floor of the port: every un-owned engine fn + all game CONTENT runs through this flat
// interpreter, so "where do interpreter instructions go?" == "which engine fn should I own next for
// perf?" (the #1 priority — owning a hot fn natively both advances 100%-PC-native AND speeds it up).
// Two cheap, exact (not sampled) histograms, gated behind g_prof_on so the hot path costs one
// predictable branch + one add when profiling is OFF, and one array increment when ON:
//   1. PC histogram  — instructions executed per 16-byte bucket over main RAM 0x80000000..0x80200000
//      (131072 buckets). Maps to functions offline via tools/prof_report.py + tomba2_funcs.txt. 16B
//      (not 256B) so a bucket base aligns with a function start — coarser buckets straddle adjacent
//      small functions and mis-attribute (a 256B bucket lumped FUN_80084110 under FUN_80084080).
//      This is the TIME profile: a hot bucket = where the CPU actually spends interpreted cycles.
//   2. CALL histogram — entries into each non-native (interpreter-run) function, counted in
//      coro_native_call's miss path. The FREQUENCY profile: a high-count target is worth owning even
//      if each call is cheap. Open-addressing, same shape as the ifn set.
// Overlay (DEMO/GAME.BIN) functions aren't in tomba2_funcs.txt; their hot addresses report raw.
int             g_prof_on = 0;                    // toggled by REPL `prof start`/`prof off`
static uint64_t g_prof_pc[1 << 17];               // 131072 buckets, 16 bytes each (aligns to fn starts)
static uint64_t g_prof_total = 0;                 // total instructions counted
static uint32_t g_prof_call_addr[1 << 14];        // call-target set (0 == empty)
static uint64_t g_prof_call_n[1 << 14];           // parallel call counts
static uint64_t g_prof_call_total = 0;            // total interpreted-fn entries counted

static inline void prof_pc_tick(uint32_t pc) {
  g_prof_pc[(pc & 0x1FFFFF) >> 4]++;
  g_prof_total++;
}
static inline void prof_call_tick(uint32_t tgt) {
  uint32_t h = (tgt * 2654435761u) >> 18, m = (1u << 14) - 1u;
  for (uint32_t i = 0; i < (1u << 14); i++) {
    uint32_t s = (h + i) & m;
    if (g_prof_call_addr[s] == tgt) { g_prof_call_n[s]++; g_prof_call_total++; return; }
    if (g_prof_call_addr[s] == 0)   { g_prof_call_addr[s] = tgt; g_prof_call_n[s] = 1; g_prof_call_total++; return; }
  }
}
void prof_start(void) {
  for (uint32_t i = 0; i < (1u << 17); i++) g_prof_pc[i] = 0;
  for (uint32_t i = 0; i < (1u << 14); i++) { g_prof_call_addr[i] = 0; g_prof_call_n[i] = 0; }
  g_prof_total = g_prof_call_total = 0;
  g_prof_on = 1;
  fprintf(stderr, "[prof] profiling started (reset)\n");
}
void prof_stop(void) { g_prof_on = 0; fprintf(stderr, "[prof] profiling stopped\n"); }

void prof_dump(const char* path) {
  // Top PC buckets (time) + top call targets (frequency), sorted desc. Simple selection of top-N
  // by repeated linear max — N is small (200), the arrays are 8K/16K, runs in a blink.
  FILE* fp = path && *path ? fopen(path, "w") : 0;
  FILE* out = fp ? fp : stderr;
  const int NPC = 200, NCALL = 200;
  fprintf(out, "# prof: %llu instructions, %llu interpreted-fn entries\n",
          (unsigned long long)g_prof_total, (unsigned long long)g_prof_call_total);
  fprintf(out, "# --- TIME (top %d PC buckets, 16B each; addr = bucket base) ---\n", NPC);
  fprintf(out, "# bucket_addr   insns      pct\n");
  // copy bucket counts so we can zero out as we extract
  for (int k = 0; k < NPC; k++) {
    uint64_t best = 0; int bi = -1;
    for (int i = 0; i < (1 << 17); i++) if (g_prof_pc[i] > best) { best = g_prof_pc[i]; bi = i; }
    if (bi < 0 || best == 0) break;
    uint32_t addr = 0x80000000u | ((uint32_t)bi << 4);
    double pct = g_prof_total ? 100.0 * (double)best / (double)g_prof_total : 0.0;
    fprintf(out, "%08X   %10llu   %5.2f%%\n", addr, (unsigned long long)best, pct);
    g_prof_pc[bi] = 0;  // consume (g_prof_on is off during dump; start re-zeros anyway)
  }
  fprintf(out, "# --- FREQUENCY (top %d interpreted-fn entry counts) ---\n", NCALL);
  fprintf(out, "# func_addr     calls      pct\n");
  for (int k = 0; k < NCALL; k++) {
    uint64_t best = 0; int bi = -1;
    for (int i = 0; i < (1 << 14); i++) if (g_prof_call_addr[i] && g_prof_call_n[i] > best) { best = g_prof_call_n[i]; bi = i; }
    if (bi < 0 || best == 0) break;
    double pct = g_prof_call_total ? 100.0 * (double)best / (double)g_prof_call_total : 0.0;
    fprintf(out, "%08X   %10llu   %5.2f%%\n", g_prof_call_addr[bi], (unsigned long long)best, pct);
    g_prof_call_n[bi] = 0;  // consume
  }
  if (fp) { fclose(fp); fprintf(stderr, "[prof] dump -> %s\n", path); }
}

static uint32_t g_callring[64];   // derail diagnostics: ring of last compiled-function entries
static int      g_callring_pos = 0;

// Invoke a call target natively if it is a BIOS vector (returns 1), else 0 (caller jumps).
// OVERRIDE SYSTEM REMOVED (2026-06-22): the interpreter NO LONGER flips into native overrides on a call.
// PSX code run via the interpreter executes pure recomp all the way down and can never re-enter native
// code (PSX never calls PC). Native code is reached only top-down, by PC calling it directly. BIOS vectors
// still route to HLE below — that is hardware emulation, not a function override.
static int coro_native_call(Core* c, uint32_t tgt) {
  if (is_bios(tgt)) {
    if (!g_ncall_init) ncall_open_once();
    uint32_t a0=c->r[4],a1=c->r[5],a2=c->r[6],a3=c->r[7];
    rec_dispatch_miss(c, tgt);
    ncall_log('B', tgt, a0,a1,a2,a3, c->r[2], c->r[3]);
    return 1;
  }
  // PLATFORM HLE (sync_overrides.cpp): PSX BIOS-library HW-sync leaves (libcd/libetc/libmdec) that
  // busy-spin on an IRQ/status bit we don't model. NOT the removed game-override table — these are
  // hardware-emulation entries only (same class as the is_bios HLE above), restricted to the
  // BIOS-library address window. A `jal` to one runs the native HLE and returns to the caller.
  { extern OverrideFn platform_hle_lookup(uint32_t);
    OverrideFn pf = platform_hle_lookup(tgt);
    if (pf) { pf(c); return 1; } }
  // No-interpreter SUBSTRATE: if `tgt` is a statically-recompiled function, run its COMPILED body
  // (rec_dispatch -> the generated addr->func_XXXX switch -> gen_func / g_override) instead of letting
  // the flat interpreter execute it. Returns "handled" so the loop resumes at the caller's return addr,
  // exactly like a native override. rec_func_index() is always -1 in the interpreter-only build, so this
  // is a no-op there (no behavior change, no #ifdef needed). The compiled body itself reaches non-
  // recompiled callees (overlays / the 495 indirect fns) back through rec_dispatch -> interp.
  // Bisect gate (PSXPORT_SUBSTRATE_LO/HI, hex KSEG0 addrs): when set, route to the compiled body only
  // for tgt in [LO,HI); outside the window fall to the interpreter. Lets us binary-search a derailing
  // compiled body without rebuilding. Unset window (LO==0 && HI==0) = route ALL recompiled targets.
  static int sg_init = 0; static uint32_t sg_lo = 0, sg_hi = 0;
  if (!sg_init) { sg_init = 1; const char* l = cfg_str("PSXPORT_SUBSTRATE_LO"); const char* h = cfg_str("PSXPORT_SUBSTRATE_HI");
                  if (l) sg_lo = (uint32_t)strtoul(l, 0, 16); if (h) sg_hi = (uint32_t)strtoul(h, 0, 16); }
  if (rec_func_index(tgt) >= 0 && (!(sg_lo | sg_hi) || (tgt >= sg_lo && tgt < sg_hi))) {
    g_callring[g_callring_pos++ & 63] = tgt;   // derail diagnostics: last compiled entries
    rec_dispatch(c, tgt); return 1;            // recompiled target -> run its COMPILED body
  }
  ifn_record(tgt, c->r[31]);   // tripwire: the interpreter is about to run this (non-native) function
  if (g_prof_on) prof_call_tick(tgt);  // perf: count entries into this un-owned (interpreted) function
  return 0;
}

// Cooperative-yield handshake (later-169). A native override sets c->coro_redirect_pc to hand control
// to a guest function that runs IN-CONTEXT (in the SAME flat interp / task run), instead of nesting a
// rec_interp via rec_dispatch (which dies on a deep yield's longjmp — later-168). After every native
// override returns, the flat loop's next pc is the redirect if set, else the override's r[31] (normal
// "return to caller"). Consuming clears it, so it never leaks past one control transfer.
void rec_coro_redirect(Core* c, uint32_t target) { c->coro_redirect_pc = target; }
static inline uint32_t coro_next_pc(Core* c) {
  if (c->coro_redirect_pc) { uint32_t p = c->coro_redirect_pc; c->coro_redirect_pc = 0; return p; }
  return c->r[31];
}

static void interp_flat(Core* c, uint32_t pc, uint32_t stop_ra) {
  // Spin detector (PSXPORT_SPINDBG): a non-yielding busy-wait in game code loops here forever
  // (never returns to the scheduler, never calls a native override that longjmps). Track the
  // pc range over a window; if we run a huge number of iterations without leaving a tiny pc
  // window, the game is busy-waiting — dump the loop range + the branch's register operands so
  // the wait condition can be identified and ported to PC.
  static int spindbg = -1;
  if (spindbg < 0) spindbg = cfg_dbg("spin") ? 1 : 0;
  unsigned long iters = 0; uint32_t lo = pc, hi = pc;
  for (;;) {
    // Exit the moment control reaches our return sentinel — by a `jr ra` (handled below) OR by a
    // tail-call's implicit return (`pc = c->r[31]` after a native override, where ra was the
    // inherited sentinel). The old recursive rec_interp returned to C on any tail-call into an
    // override; the flat loop needs this top check to match it. stop_ra (CORO_SENTINEL 0xDEAD0000)
    // is a poison address, never real code, so this is a no-op for normal flow in both callers.
    if (pc == stop_ra) return;
    if (spindbg) {
      if (pc < lo) lo = pc; if (pc > hi) hi = pc;
      if (++iters >= 80000000UL) {
        fprintf(stderr, "[spindbg] busy-loop: pc window 0x%08X..0x%08X (cur 0x%08X) "
                "regs: v0=%08X v1=%08X a0=%08X a1=%08X t0=%08X t1=%08X s0=%08X s1=%08X\n",
                lo, hi, pc, c->r[2], c->r[3], c->r[4], c->r[5], c->r[8], c->r[9],
                c->r[16], c->r[17]);
        // CD-streaming contract (FUN_8001cfc8 task slot 2): start/end LBA = task2 obj
        // (0x801fe0e0) +0x54/+0x58, dest/words at _DAT_1f8001f8/f4, plus the stream flags.
        fprintf(stderr, "[spindbg]   stream: startLBA=%u endLBA=%u chan=%u be0e4=0x%02X "
                "dest=0x%08X words=%u f0=%u\n",
                c->mem_r32(0x801fe134), c->mem_r32(0x801fe138), c->mem_r8(0x801fe146),
                c->mem_r8(0x800be0e4), c->mem_r32(0x1f8001f8), c->mem_r32(0x1f8001f4), c->mem_r32(0x1f8001f0));
        iters = 0; lo = hi = pc;
      }
    }
    g_interp_pc = pc;
    // PSXPORT_PCTRAP=0xADDR — when the interpreter first reaches ADDR, dump the guest call chain (ra + a
    // wide stack scan incl. OVERLAY code 0x80100000..0x80200000) so we can find the native->interpreted
    // handoff for a still-PSX path (e.g. the field render driver). later-242 (RE tool, not behavior).
    { static uint32_t trap = 0xFFFFFFFFu; static long skipn = 0;
      if (trap == 0xFFFFFFFFu) { const char* s = cfg_str("PSXPORT_PCTRAP"); trap = s ? (uint32_t)strtoul(s,0,0) : 0;
        const char* k = cfg_str("PSXPORT_PCTRAP_SKIP"); skipn = k ? strtol(k,0,0) : 0; }
      if (trap && pc == trap) { static long hit = 0; if (hit++ == skipn) {
        fprintf(stderr, "[pctrap] reached 0x%08X  ra=0x%08X sp=0x%08X a0=0x%08X\n", pc, c->r[31], c->r[29], c->r[4]);
        uint32_t sp = c->r[29]; int shown = 0;
        for (uint32_t a = sp; a < sp + 1024 && shown < 24; a += 4) { uint32_t w = c->mem_r32(a); uint32_t k = w & 0x1FFFFFFF;
          if (k >= 0x10000 && k < 0x200000 && (w & 3) == 0) { fprintf(stderr, "    [sp+0x%03X] 0x%08X\n", a - sp, w); shown++; } }
        fflush(stderr); } } }
    // DIAG (debug chan `fadeshot`): every recomp screen-fade call FUN_8007E9C8(color=a0) — capture s_tex
    // and log color+ra, to see the intro menu->cutscene transition's "two fade-ins" render state deterministically.
    if (pc == 0x8007E9C8u) {
      static int fs = -2; if (fs == -2) fs = cfg_dbg("fadeshot") ? 1 : 0;
      if (fs) { void gpu_vk_shot(Core*, const char*); static int fn = 0;
        fprintf(stderr, "[fadeshot] call=%d color=0x%06X ra=0x%08X\n", fn, c->r[4] & 0xffffff, c->r[31]);
        if (fn < 120) { char p[128]; snprintf(p, sizeof p, "scratch/screenshots/fade_%03d.ppm", fn); gpu_vk_shot(c, p); }
        fn++; }
    }
    // PSXPORT_DEBUG=keyon (oracle, temporary): trace every libsnd voice keyon 0x800939A0
    // (a0=seq|chan<<8, a1=vab id, a2=program, a3=note, sp+16=velocity). Reveals which sequences/
    // instruments/notes actually compose a song — ground truth for the offline snd_render tool.
    if (pc == 0x800939A0u) {
      static int kon = -2; if (kon == -2) kon = cfg_dbg("keyon") ? 1 : 0;
      if (kon) fprintf(stderr, "[keyon] seq=%u chan=%u vab=%d prog=%d note=%u vel=%u\n",
                       c->r[4] & 0xff, (c->r[4] >> 8) & 0xff, (int)(int16_t)c->r[5],
                       (int)(int16_t)c->r[6], c->r[7] & 0xff, c->mem_r32(c->r[29] + 16));
    }
    // PSXPORT_DEBUG=bgmreq: trace the game's BGM trigger sound_play_bgm 0x80074BF8 (a0=idx; low7=song,
    // bit7 set => loop). Reveals which song the GAME LOGIC actually requests per area/dialogue — the
    // signal the native field_bgm_director currently ignores (it hardcodes song 8).
    if (pc == 0x80074BF8u || pc == 0x80074E48u) {
      static int br = -2; if (br == -2) br = cfg_dbg("bgmreq") ? 1 : 0;
      if (br) { if (pc == 0x80074E48u) fprintf(stderr, "[bgmreq] sound_stop_bgm() ra=%08X\n", c->r[31]);
                else fprintf(stderr, "[bgmreq] sound_play_bgm(idx=%u song=%u loop=%d) ra=%08X\n",
                             c->r[4], c->r[4] & 0x7f, (c->r[4] & 0x80) == 0, c->r[31]); }
    }
    // PSXPORT_DEBUG=demoflag: trace which demo-flag (0x1f80019a) READER PCs execute (find DEMO-text drawer).
    if (pc == 0x80026874u || pc == 0x80052208u || pc == 0x800522b0u || pc == 0x80075834u || pc == 0x800788ccu) {
      static int df = -2; if (df == -2) df = cfg_dbg("demoflag") ? 1 : 0;
      if (df) { static unsigned n[5]={0,0,0,0,0}; int k = pc==0x80026874u?0:pc==0x80052208u?1:pc==0x800522b0u?2:pc==0x80075834u?3:4;
                if (++n[k] <= 2) fprintf(stderr, "[demoflag] reader @%08X hit (flag=%02X) ra=%08X\n", pc, c->mem_r8(0x1f80019au), c->r[31]); }
    }
    // PSXPORT_DEBUG=septrace: trace the libsnd SEP event dispatcher 0x80091460 — at 0x800914d0 the
    // status byte (s2=r18) has just been read from the track stream pointer (a3=r7, already +1). Log
    // the byte ADDRESS and value per event = the game's exact event walk (byte-consumption oracle for
    // aligning our na_seq_render parser). r17=seq, r16/.. set later; r7-1 = the status byte's address.
    if (pc == 0x800914D0u) {
      static int stc = -2; if (stc == -2) stc = cfg_dbg("septrace") ? 1 : 0;
      if (stc) fprintf(stderr, "[septrace] @%08X status=%02X\n", c->r[7] - 1, c->r[18] & 0xff);
    }
    // PSXPORT_DEBUG=tickdbg: where does the recomp libsnd sequencer stall? trace the per-vblank tick
    // wrapper 0x800909C0, SsSeqCalled 0x80090BD0, and the SEP event dispatcher 0x80091460. If the tick
    // runs but the dispatcher never does, the sequence clock isn't advancing (frozen sequencer).
    if (pc == 0x800909C0u || pc == 0x80090BD0u || pc == 0x80091460u) {
      static int td = -2; if (td == -2) td = cfg_dbg("tickdbg") ? 1 : 0;
      if (td) { static unsigned ct[3]={0,0,0}; int k = pc==0x800909C0u?0 : pc==0x80090BD0u?1:2;
                if (++ct[k] <= 3 || ct[k]%200==0)
                  fprintf(stderr, "[tickdbg] %s count=%u\n",
                          pc==0x800909C0u?"tick(909C0)":pc==0x80090BD0u?"SsSeqCalled(90BD0)":"dispatch(91460)", ct[k]); }
    }
    // PSXPORT_DEBUG=seqopen: trace SsSeqOpen 0x80090210 (a0=SEP addr, a1=VAB id) — the game's intended
    // SEQ->VAB binding (which we currently guess). a2/a3 = seq/sub indices. Reveals which VAB each song
    // is authored against, the missing piece for correct instruments/pitch.
    if (pc == 0x80090210u) {
      static int so = -2; if (so == -2) so = cfg_dbg("seqopen") ? 1 : 0;
      if (so) fprintf(stderr, "[seqopen] SsSeqOpen(sep=%08X vab=%d) ra=%08X\n",
                      c->r[4], (int)(int16_t)c->r[5], c->r[31]);
    }
    // PSXPORT_DEBUG=seqplay: trace SsSeqPlay 0x80090560 (a0=seq handle) — which sequences are played.
    if (pc == 0x80090560u) {
      static int sp = -2; if (sp == -2) sp = cfg_dbg("seqplay") ? 1 : 0;
      if (sp) fprintf(stderr, "[seqplay] SsSeqPlay(handle=%d, mode=%d, loop=%d)\n",
                      (int)(int16_t)c->r[4], c->r[5], c->r[6]);
    }
    // PSXPORT_DEBUG=banksel: trace the libsnd bank-select event handler 0x8008e390 (sets channel
    // slot[0x26]=VAB from the stream). a0=seq, a1=chan. Reveals whether it runs for note channels.
    if (pc == 0x8008e390u) {
      static int bs = -2; if (bs == -2) bs = cfg_dbg("banksel") ? 1 : 0;
      if (bs) { uint32_t cap = c->mem_r32(0x80104c30u + (c->r[4] & 0xffff) * 4);
                uint32_t cs = cap + (c->r[5] & 0xffff) * 176;
                uint32_t dp = c->mem_r32(cs);
                fprintf(stderr, "[banksel] seq=%u chan=%u streambyte=0x%02x (-> slot[0x26])\n",
                        c->r[4] & 0xffff, c->r[5] & 0xffff, c->mem_r8(dp)); }
    }
    if (g_prof_on) prof_pc_tick(pc);   // perf profiler: instructions-per-PC-bucket (time histogram)
    // PSXPORT_SPRITEDBG: when the sprite-flush routine copies the red-quad clut template
    // (0x7EF71100) into the OT (store at 0x8007E67C), dump the renderer's working registers so
    // the owning sprite object ($a3/$t5) and its descriptor list ($a2) can be identified.
    // PSXPORT_TEXTDBG: log every call to the text/sprite-row drawer 0x8007E998 (a0=x, a1=y, a3=glyph),
    // with the caller ra — to trace which overlay code draws a given 2D text/banner element.
    if (pc == 0x8007E998 && cfg_dbg("text")) {
      static int n = 0;
      if (n++ < 30)
        fprintf(stderr, "[textdbg] 8007E998(x=%d y=%d a2=%08X a3=%08X) ra=%08X stage=%08X\n",
                (int)(int16_t)c->r[4], (int)(int16_t)c->r[5], c->r[6], c->r[7], c->r[31],
                c->mem_r32(0x801fe00c));
    }
    uint32_t in = c->mem_r32(pc);
    uint32_t op = in >> 26;
    // SUBSTRATE derail reporter (PSXPORT_DERAIL): a compiled<->interp return that goes wrong lands the
    // interp on garbage (insn 0xFFFFFFFF). Report the exact PC + regs ONCE and stop the run (instead of
    // spinning 12M "bad opcode" lines), so the offending function's broken return can be identified.
    if (op == 0x3F && cfg_dbg("derail")) {
      fprintf(stderr, "[derail] pc=%08X in=%08X  ra=%08X sp=%08X gp=%08X  stop_ra=%08X\n",
              pc, in, c->r[31], c->r[29], c->r[28], stop_ra);
      for (int k = 0; k < 16; k++) fprintf(stderr, "  stk[%2d] @%08X = %08X\n", k, c->r[29] + k*4, c->mem_r32(c->r[29] + k*4));
      fprintf(stderr, "[derail] last compiled entries (newest last):\n");
      for (int k = 24; k >= 1; k--) { uint32_t a = g_callring[(g_callring_pos - k) & 63]; if (a) fprintf(stderr, "  %08X\n", a); }
      fflush(stderr); abort();
    }
    ldhaz_step(in, pc);                              // load-delay hazard detector (execution order)

    // `break` is a program trap. We HLE the BIOS, so there is no exception handler to resume
    // into — the trap ENDS this run. In particular crt0 (0x800896E0) is `jal main; break`: on
    // real PSX main never returns, but our native main (ov_game_main) returns after N headless
    // frames, so control reaches that terminal break. The old recursive rec_interp escaped it by
    // chance (it returned to C on the next `jr ra`, which fell through into a halt loop); the flat
    // loop must treat the break itself as the halt. (A `break` never executes on a hot path here —
    // the field run hits exactly one, this terminal — so ending the run on it is correct.)
    if (op == 0x00 && FN(in) == 0x0D) { rec_break(c, (in >> 6) & 0xFFFFF); return; }

    if (op == 0x02 || op == 0x03) {                  // j / jal
      uint32_t tgt = TGT(in, pc);
      if (op == 0x03) { c->r[31] = pc + 8; trace_call(pc, tgt); }  // jal: link + optional call trace
      ldhaz_step(c->mem_r32(pc + 4), pc + 4);            // delay slot executes next
      exec_simple(c, c->mem_r32(pc + 4));               // delay slot
      // A native override / BIOS vector must win on EITHER a `jal` call or a tail-`j` into it,
      // else the flat interpreter re-runs a function the PC side owns (e.g. the LZ decompressor
      // 0x80044D8C) and can diverge from it. coro_native_call only fires for exact override/BIOS
      // addresses, so a plain local `j` (label inside interpreted code) falls through unchanged.
      if (coro_native_call(c, tgt)) { pc = coro_next_pc(c); continue; }
      pc = tgt; continue;                            // flat call/jump
    }
    if (op == 0x00 && (FN(in) == 0x08 || FN(in) == 0x09)) {  // jr / jalr
      uint32_t tgt = c->r[RS(in)];
      uint32_t link = pc + 8, rd = RD(in);
      int is_jalr = FN(in) == 0x09, is_ra = RS(in) == 31;
      ldhaz_step(c->mem_r32(pc + 4), pc + 4);            // delay slot executes next
      exec_simple(c, c->mem_r32(pc + 4));               // delay slot
      if (is_jalr) {
        if (rd) c->r[rd] = link;
        trace_call(pc, tgt);                         // optional call trace (PSXPORT_INTERP_TRACE)
        if (coro_native_call(c, tgt)) { pc = coro_next_pc(c); continue; }
        pc = tgt; continue;                          // flat indirect call
      }
      if (is_ra) {                                   // return
        if (tgt == stop_ra) return;                  // returned to our sentinel -> this run is done
        pc = tgt; continue;                          // flat return up the PSX call chain
      }
      if (coro_native_call(c, tgt)) { pc = coro_next_pc(c); continue; }  // computed tail-call
      trace_call(pc, tgt);                           // computed jump (switch table / jr-dispatch) — traced too
      pc = tgt; continue;                            // computed jump (switch table etc.)
    }
    if (op == 0x01 || op == 0x04 || op == 0x05 || op == 0x06 || op == 0x07) {  // branches
      int t;
      uint32_t rs = RS(in), rt = RT(in);
      uint32_t tgt = pc + 4 + (SIMM(in) << 2);
      if (op == 0x04) t = (c->r[rs] == c->r[rt]);
      else if (op == 0x05) t = (c->r[rs] != c->r[rt]);
      else if (op == 0x06) t = ((int32_t)c->r[rs] <= 0);
      else if (op == 0x07) t = ((int32_t)c->r[rs] > 0);
      else {
        uint32_t sub = rt;
        t = (sub & 1) ? ((int32_t)c->r[rs] >= 0) : ((int32_t)c->r[rs] < 0);
        if (sub & 0x10) c->r[31] = pc + 8;
      }
      ldhaz_step(c->mem_r32(pc + 4), pc + 4);            // delay slot executes next
      exec_simple(c, c->mem_r32(pc + 4));
      pc = t ? tgt : pc + 8;
      continue;
    }
    exec_simple(c, in);
    pc += 4;
  }
}

// ---- The two public entry points: thin wrappers over the single flat loop -----------------
// They differ only in the return sentinel. Folding the old recursive rec_interp into the flat
// loop removes the second control-flow loop (and the index-vs-address override duality that went
// with it) — leaving ONE interpreter.

// A cooperative TASK: native_boot.c enters its top function with ra=CORO_SENTINEL, then the loop
// runs until that function returns (jr ra -> sentinel == task ended) or a native override
// (ov_yield/CD) longjmps back to the scheduler.
void rec_coro_run(Core* c, uint32_t pc) { interp_flat(c, pc, CORO_SENTINEL); }

// A SYNCHRONOUS nested call — call_addr's old recursive job, plus rec_super_call (interpret the
// original PSX body for the A/B oracle) and the dispatch-miss RAM-code path. It must run the
// target to completion and return to its C caller. We push the sentinel as the return address
// and run flat until the target returns to it; the target's own prologue/epilogue saves and
// restores whatever ra holds, so on exit we put the caller's ra back — matching the net effect
// of the old recursive rec_interp exactly. Nesting (e.g. an override calling rec_super_call mid
// task) is safe: each invocation's PSX frames sit above the caller's, so only the target's own
// `jr ra` reaches the sentinel and ends this run.
void rec_interp(Core* c, uint32_t pc) {
  uint32_t saved_ra = c->r[31];
  c->r[31] = CORO_SENTINEL;
  interp_flat(c, pc, CORO_SENTINEL);
  c->r[31] = saved_ra;
}

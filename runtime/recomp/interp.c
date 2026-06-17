// Hybrid fallback interpreter for non-recompiled code (overlays + computed-jump targets).
//
// The static recompiler covers MAIN.EXE's resident text. Code overlays (START/OPN/GAME/...
// .BIN) are loaded from disc at runtime ABOVE that text (0x80106xxx etc.) and swap at shared
// addresses, so they can't all be statically recompiled ahead of time. This interpreter runs
// any such code directly from g_ram, using the SAME runtime (mem/gte/hle/threads) and the
// SAME instruction semantics as the emitter (tools/recomp/emit.py) so interpreted and
// recompiled code are bit-identical. It is the rec_dispatch-miss fallback: a `jal`/`jr`/`jalr`
// into non-recompiled RAM enters rec_interp; a call back into a recompiled function routes to
// rec_dispatch. (Also resolves the in-function jump-table misses in MAIN.EXE — printf
// 0x8009A76C, SetVideoMode 0x80091D70 — by interpreting from the computed target in RAM.)
//
// Faithful-first simplifications match the emitter: no load-delay slot; add==addu; signed
// div/mult via cpu_div/mult helpers; GTE via gte_op/gte_read/write.
#include "r3000.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>

void rec_dispatch(R3000* c, uint32_t addr);
void rec_syscall(R3000* c, uint32_t code);
void rec_break(R3000* c, uint32_t code);
void rec_interp(R3000* c, uint32_t pc);

// Diagnostics: the PC currently being interpreted (read by the watchdog on a stall to report
// WHERE the interpreter is spinning). Optional call trace (PSXPORT_INTERP_TRACE=<path>) logs
// jal/jalr targets — used to follow the boot stub and find where it wedges.
volatile uint32_t g_interp_pc = 0;
static FILE* g_trace_fp = 0;
void interp_trace_open(const char* path) {
  if (path && *path) { g_trace_fp = fopen(path, "w"); if (!g_trace_fp) perror(path); }
}
static inline void trace_call(uint32_t from, uint32_t to) {
  if (g_trace_fp) fprintf(g_trace_fp, "%08X -> %08X\n", from, to);
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

// ---- R3000 load-delay hazard DETECTOR (PSXPORT_LDHAZARD) ------------------------------------
// Our interpreter omits the load-delay slot (interp.c head): a load's target reg is visible to
// the immediately-following instruction, whereas real R3000 (and the mednafen oracle) make that
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

// Is `addr` a recompiled function or a BIOS vector? Then a call routes to rec_dispatch;
// otherwise it is non-recompiled RAM code we interpret.
int rec_func_index(uint32_t addr);
static int is_recompiled(uint32_t a) {
  uint32_t p = a & 0x1FFFFFFF;
  return rec_func_index(a) >= 0 || p == 0xA0 || p == 0xB0 || p == 0xC0;
}
// Raw-address override table for NON-recompiled (interpreted) code — the boot stub and overlays.
// rec_set_override / g_override are keyed by recompiled-function INDEX (rec_func_index), so they
// can't target stub/overlay addresses (index == -1). This parallel table is keyed by raw address
// and consulted by call_addr below, letting native_stub.c replace the stub's libcd/libetc waits.
#define MAX_IOV 256
static struct { uint32_t addr; OverrideFn fn; int isauto; } g_iov[MAX_IOV];
static int g_iov_n;
static void iov_add(uint32_t addr, OverrideFn fn, int isauto) {
  for (int i = 0; i < g_iov_n; i++) if (g_iov[i].addr == addr) { g_iov[i].fn = fn; g_iov[i].isauto = isauto; return; }
  if (g_iov_n < MAX_IOV) { g_iov[g_iov_n].addr = addr; g_iov[g_iov_n].fn = fn; g_iov[g_iov_n].isauto = isauto; g_iov_n++; }
}
void rec_set_interp_override(uint32_t addr, OverrideFn fn) { iov_add(addr, fn, 0); }
// Auto (scan-registered) overlay override: the engine scans a freshly-loaded overlay ONCE for known
// library-fn signatures and registers each here (isauto=1). Cleared on the next overlay load (the
// address is only stable while that code is resident — per-scene reuse). Done at load time, so the
// per-call path stays a plain g_iov lookup — no per-call classification (that was far too slow).
void rec_set_interp_override_auto(uint32_t addr, OverrideFn fn) { iov_add(addr, fn, 1); }
static OverrideFn interp_override_for(uint32_t a) {
  for (int i = 0; i < g_iov_n; i++) if (g_iov[i].addr == a) return g_iov[i].fn;
  return 0;
}
// Drop every scan-registered (overlay) override, so a re-scan of the newly loaded overlay re-owns
// the (possibly relocated) library fns. Called by rec_overlay_loaded before the engine re-scans.
static void iov_flush_auto(void) {
  int w = 0;
  for (int i = 0; i < g_iov_n; i++) if (!g_iov[i].isauto) g_iov[w++] = g_iov[i];
  g_iov_n = w;
}
// The engine's "an overlay was just loaded into [base,base+size)" hook (scans for owned library fns).
static void (*g_overlay_load_hook)(uint32_t base, uint32_t size);
void rec_set_overlay_load_hook(void (*fn)(uint32_t, uint32_t)) { g_overlay_load_hook = fn; }
void rec_overlay_loaded(uint32_t base, uint32_t size) {
  iov_flush_auto();                                // stale overlay overrides gone; re-own from scratch
  if (g_overlay_load_hook) g_overlay_load_hook(base, size);
}
// Public accessor so rec_dispatch_miss can apply an interp override at the moment it would ENTER
// the interpreter (a recompiled `jalr`/dispatch into a non-recompiled stub fn lands here, not via
// call_addr). Keyed by full KSEG0 address.
OverrideFn rec_interp_override_for(uint32_t a) { return interp_override_for(a); }
static void call_addr(R3000* c, uint32_t a) {
  OverrideFn ov = interp_override_for(a);
  if (ov) { ov(c); return; }                 // native replacement (returns to caller, sets v0)
  if (is_recompiled(a)) rec_dispatch(c, a);
  else rec_interp(c, a);
}

// Native override registered for `addr` (rec_set_override), or NULL. Used by the coroutine
// interpreter to invoke hand-written overrides (CD, yield, ...) instead of flat-interpreting.
extern OverrideFn g_override[];
static OverrideFn override_for(uint32_t a) {
  int i = rec_func_index(a);
  return i >= 0 ? g_override[i] : 0;
}
static int is_bios(uint32_t a) { uint32_t p = a & 0x1FFFFFFF; return p==0xA0||p==0xB0||p==0xC0; }

// Execute one non-control instruction (delay-slot-safe; no branches/jumps/loads-delay).
static void exec_simple(R3000* c, uint32_t in) {
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
    case 0x20: W(RT(in), (uint32_t)(int8_t)mem_r8(c->r[RS(in)] + SIMM(in))); break;   // lb
    case 0x24: W(RT(in), (uint32_t)mem_r8(c->r[RS(in)] + SIMM(in))); break;           // lbu
    case 0x21: W(RT(in), (uint32_t)(int16_t)mem_r16(c->r[RS(in)] + SIMM(in))); break; // lh
    case 0x25: W(RT(in), (uint32_t)mem_r16(c->r[RS(in)] + SIMM(in))); break;          // lhu
    case 0x23: W(RT(in), mem_r32(c->r[RS(in)] + SIMM(in))); break;                    // lw
    case 0x22: W(RT(in), mem_lwl(c->r[RT(in)], c->r[RS(in)] + SIMM(in))); break;      // lwl
    case 0x26: W(RT(in), mem_lwr(c->r[RT(in)], c->r[RS(in)] + SIMM(in))); break;      // lwr
    case 0x28: mem_w8(c->r[RS(in)] + SIMM(in), (uint8_t)c->r[RT(in)]); break;         // sb
    case 0x29: mem_w16(c->r[RS(in)] + SIMM(in), (uint16_t)c->r[RT(in)]); break;       // sh
    case 0x2B: mem_w32(c->r[RS(in)] + SIMM(in), c->r[RT(in)]); break;                 // sw
    case 0x2A: mem_swl(c->r[RS(in)] + SIMM(in), c->r[RT(in)]); break;                 // swl
    case 0x2E: mem_swr(c->r[RS(in)] + SIMM(in), c->r[RT(in)]); break;                 // swr
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
    case 0x32: gte_write_data(RT(in), mem_r32(c->r[RS(in)] + SIMM(in))); break;       // lwc2
    case 0x3A: mem_w32(c->r[RS(in)] + SIMM(in), gte_read_data(RT(in))); break;        // swc2
    default: fprintf(stderr, "[interp] bad opcode 0x%02X @ insn 0x%08X\n", op, in); break;
  }
}

// Interpret from `pc` until the function returns (jr ra) or tail-jumps into recompiled code.
void rec_interp(R3000* c, uint32_t pc) {
  for (;;) {
    g_interp_pc = pc;
    uint32_t in = mem_r32(pc);
    uint32_t op = in >> 26;

    if (op == 0x02 || op == 0x03) {                  // j / jal
      uint32_t tgt = TGT(in, pc);
      if (op == 0x03) c->r[31] = pc + 8;             // jal links ra
      exec_simple(c, mem_r32(pc + 4));               // delay slot
      if (op == 0x03) { trace_call(pc, tgt); call_addr(c, tgt); pc += 8; continue; }  // jal: call, resume after DS
      if (is_recompiled(tgt)) { rec_dispatch(c, tgt); return; }  // j tail-call into recomp
      pc = tgt; continue;                            // j within interpreted code
    }
    if (op == 0x00 && (FN(in) == 0x08 || FN(in) == 0x09)) {  // jr / jalr
      uint32_t tgt = c->r[RS(in)];
      uint32_t link = (FN(in) == 0x09) ? (pc + 8) : 0;
      uint32_t rd = RD(in);
      exec_simple(c, mem_r32(pc + 4));               // delay slot
      if (FN(in) == 0x09) {                          // jalr: link + call + resume
        if (rd) c->r[rd] = link;
        trace_call(pc, tgt); call_addr(c, tgt); pc += 8; continue;
      }
      if (RS(in) == 31) return;                      // jr ra: return to caller
      if (is_recompiled(tgt)) { rec_dispatch(c, tgt); return; }  // computed tail-call
      pc = tgt; continue;                            // computed jump within interpreted code
    }
    if (op == 0x01 || op == 0x04 || op == 0x05 || op == 0x06 || op == 0x07) {  // branches
      int t;
      uint32_t rs = RS(in), rt = RT(in);
      uint32_t tgt = pc + 4 + (SIMM(in) << 2);
      if (op == 0x04) t = (c->r[rs] == c->r[rt]);              // beq
      else if (op == 0x05) t = (c->r[rs] != c->r[rt]);         // bne
      else if (op == 0x06) t = ((int32_t)c->r[rs] <= 0);       // blez
      else if (op == 0x07) t = ((int32_t)c->r[rs] > 0);        // bgtz
      else {                                                   // REGIMM: bltz/bgez/bltzal/bgezal
        uint32_t sub = rt;
        t = (sub & 1) ? ((int32_t)c->r[rs] >= 0) : ((int32_t)c->r[rs] < 0);
        if (sub & 0x10) c->r[31] = pc + 8;                     // *al link
      }
      exec_simple(c, mem_r32(pc + 4));               // delay slot
      pc = t ? tgt : pc + 8;
      continue;
    }
    // non-control instruction
    exec_simple(c, in);
    pc += 4;
  }
}

// ---- Flat coroutine interpreter (rec_coro_run) -------------------------------------------
// Runs one cooperative task as a resumable coroutine WITHOUT using the host C stack for PSX
// calls (so a yield can longjmp out and a later call resume mid-chain — see native_boot.c).
// Unlike rec_interp (which mirrors PSX calls onto the C stack and treats `jr ra` as a C
// return), this is FLAT: jal/jalr set the link reg and JUMP (the called code maintains the PSX
// stack in g_ram itself); `jr` JUMPS to its target and keeps looping. The loop only exits when
// `jr ra` targets the sentinel 0xDEAD0000 (the task's top-level function returned == task
// ended). Native overrides (yield, CD, ...) and BIOS vectors are still invoked as C (an
// override may longjmp out, e.g. the yield); everything else is interpreted flat. This is the
// ucontext-free coroutine model: PSX state (PC via the link/stack, regs, SP-in-g_ram) is all
// that a yield needs to save/restore.
#define CORO_SENTINEL 0xDEAD0000u
void rec_dispatch_miss(R3000* c, uint32_t addr);

// Invoke a call target natively if it is an override/BIOS (returns 1), else 0 (caller jumps).
static int coro_native_call(R3000* c, uint32_t tgt) {
  OverrideFn ov = override_for(tgt);              // recompiled-fn-index override (resident fns)
  if (!ov) ov = interp_override_for(tgt);         // raw-address override + autodetect (overlay fns)
  if (ov) { ov(c); return 1; }
  if (is_bios(tgt)) { rec_dispatch_miss(c, tgt); return 1; }
  return 0;
}

void rec_coro_run(R3000* c, uint32_t pc) {
  // Spin detector (PSXPORT_SPINDBG): a non-yielding busy-wait in game code loops here forever
  // (never returns to the scheduler, never calls a native override that longjmps). Track the
  // pc range over a window; if we run a huge number of iterations without leaving a tiny pc
  // window, the game is busy-waiting — dump the loop range + the branch's register operands so
  // the wait condition can be identified and ported to PC.
  static int spindbg = -1;
  if (spindbg < 0) spindbg = cfg_dbg("spin") ? 1 : 0;
  unsigned long iters = 0; uint32_t lo = pc, hi = pc;
  for (;;) {
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
                mem_r32(0x801fe134), mem_r32(0x801fe138), mem_r8(0x801fe146),
                mem_r8(0x800be0e4), mem_r32(0x1f8001f8), mem_r32(0x1f8001f4), mem_r32(0x1f8001f0));
        iters = 0; lo = hi = pc;
      }
    }
    g_interp_pc = pc;
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
                mem_r32(0x801fe00c));
    }
    uint32_t in = mem_r32(pc);
    uint32_t op = in >> 26;
    ldhaz_step(in, pc);                              // load-delay hazard detector (execution order)

    if (op == 0x02 || op == 0x03) {                  // j / jal
      uint32_t tgt = TGT(in, pc);
      if (op == 0x03) c->r[31] = pc + 8;
      ldhaz_step(mem_r32(pc + 4), pc + 4);            // delay slot executes next
      exec_simple(c, mem_r32(pc + 4));               // delay slot
      // A native override / BIOS vector must win on EITHER a `jal` call or a tail-`j` into it,
      // else the flat interpreter re-runs a function the PC side owns (e.g. the LZ decompressor
      // 0x80044D8C) and can diverge from it. coro_native_call only fires for exact override/BIOS
      // addresses, so a plain local `j` (label inside interpreted code) falls through unchanged.
      if (coro_native_call(c, tgt)) { pc = c->r[31]; continue; }
      pc = tgt; continue;                            // flat call/jump
    }
    if (op == 0x00 && (FN(in) == 0x08 || FN(in) == 0x09)) {  // jr / jalr
      uint32_t tgt = c->r[RS(in)];
      uint32_t link = pc + 8, rd = RD(in);
      int is_jalr = FN(in) == 0x09, is_ra = RS(in) == 31;
      ldhaz_step(mem_r32(pc + 4), pc + 4);            // delay slot executes next
      exec_simple(c, mem_r32(pc + 4));               // delay slot
      if (is_jalr) {
        if (rd) c->r[rd] = link;
        if (coro_native_call(c, tgt)) { pc = c->r[31]; continue; }
        pc = tgt; continue;                          // flat indirect call
      }
      if (is_ra) {                                   // return
        if (tgt == CORO_SENTINEL) return;            // task's top function returned -> ended
        pc = tgt; continue;                          // flat return up the PSX call chain
      }
      if (coro_native_call(c, tgt)) { pc = c->r[31]; continue; }  // computed tail-call
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
      ldhaz_step(mem_r32(pc + 4), pc + 4);            // delay slot executes next
      exec_simple(c, mem_r32(pc + 4));
      pc = t ? tgt : pc + 8;
      continue;
    }
    exec_simple(c, in);
    pc += 4;
  }
}

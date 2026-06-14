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
#include <stdio.h>

void rec_dispatch(R3000* c, uint32_t addr);
void rec_syscall(R3000* c, uint32_t code);
void rec_break(R3000* c, uint32_t code);
void rec_interp(R3000* c, uint32_t pc);

#define RS(i)  (((i) >> 21) & 31)
#define RT(i)  (((i) >> 16) & 31)
#define RD(i)  (((i) >> 11) & 31)
#define SH(i)  (((i) >> 6) & 31)
#define FN(i)  ((i) & 63)
#define IMM(i) ((uint32_t)(uint16_t)(i))
#define SIMM(i)((uint32_t)(int32_t)(int16_t)(i))
#define TGT(i, pc) (((pc) & 0xF0000000u) | (((i) & 0x03FFFFFFu) << 2))

#define W(n, v) do { uint32_t _n = (n); if (_n) c->r[_n] = (v); } while (0)

// Is `addr` a recompiled function or a BIOS vector? Then a call routes to rec_dispatch;
// otherwise it is non-recompiled RAM code we interpret.
int rec_func_index(uint32_t addr);
static int is_recompiled(uint32_t a) {
  uint32_t p = a & 0x1FFFFFFF;
  return rec_func_index(a) >= 0 || p == 0xA0 || p == 0xB0 || p == 0xC0;
}
static void call_addr(R3000* c, uint32_t a) {
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
    uint32_t in = mem_r32(pc);
    uint32_t op = in >> 26;

    if (op == 0x02 || op == 0x03) {                  // j / jal
      uint32_t tgt = TGT(in, pc);
      if (op == 0x03) c->r[31] = pc + 8;             // jal links ra
      exec_simple(c, mem_r32(pc + 4));               // delay slot
      if (op == 0x03) { call_addr(c, tgt); pc += 8; continue; }  // jal: call, resume after DS
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
        call_addr(c, tgt); pc += 8; continue;
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
  OverrideFn ov = override_for(tgt);
  if (ov) { ov(c); return 1; }
  if (is_bios(tgt)) { rec_dispatch_miss(c, tgt); return 1; }
  return 0;
}

void rec_coro_run(R3000* c, uint32_t pc) {
  for (;;) {
    uint32_t in = mem_r32(pc);
    uint32_t op = in >> 26;

    if (op == 0x02 || op == 0x03) {                  // j / jal
      uint32_t tgt = TGT(in, pc);
      if (op == 0x03) c->r[31] = pc + 8;
      exec_simple(c, mem_r32(pc + 4));               // delay slot
      if (op == 0x03 && coro_native_call(c, tgt)) { pc = c->r[31]; continue; }
      pc = tgt; continue;                            // flat call/jump
    }
    if (op == 0x00 && (FN(in) == 0x08 || FN(in) == 0x09)) {  // jr / jalr
      uint32_t tgt = c->r[RS(in)];
      uint32_t link = pc + 8, rd = RD(in);
      int is_jalr = FN(in) == 0x09, is_ra = RS(in) == 31;
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
      exec_simple(c, mem_r32(pc + 4));
      pc = t ? tgt : pc + 8;
      continue;
    }
    exec_simple(c, in);
    pc += 4;
  }
}

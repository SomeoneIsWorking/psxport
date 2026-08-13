// oracle_spike.c — milestone 1 of `docs/plans/oracle-against-beetle.md`, and its own proof.
//
// WHAT THIS PROVES: that the vendored Mednafen PSX CPU, hosted without `libretro.c`, executes MIPS
// instructions we inject into a RAM image and produces the register and memory results we can compute by
// hand. WHAT IT DOES NOT PROVE: anything at all about our port. No comparison happens here. A working
// oracle is a working reference, not a verified port — the plan says so and it stays true until
// milestone 2 puts one window through both sides.
//
// WHY IT RUNS TWO CLASSES OF PROGRAM. A checker that has only ever seen the case it expects is not an
// instrument, and this workspace has been burned by exactly that (`docs/findings/`: a discriminator that
// scored 25 on the negative case and 0 on the positive). So:
//   * the POSITIVE program computes a value only a working CPU can produce, and every register is
//     asserted against a constant worked out from the MIPS reference by hand, right here in the comments;
//   * the NEGATIVE program reads a GPU register, and the run must report ORACLE_STOP_HARDWARE at that
//     exact address. If the shim silently returned 0 for device reads, this case would come back as a
//     clean window and the compare would later run over instructions nobody executed.
// Neither case can pass by the oracle doing nothing: an oracle that executes zero instructions fails the
// positive (registers stay 0) and fails the negative (no hardware access is ever seen).
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "oracle_shim.h"

// ── MIPS R3000A encoders, so the fixture reads as instructions rather than hex ────────────────────
#define R_ZERO 0
#define R_T0   8
#define R_T1   9
#define R_T2  10
#define R_T3  11
#define R_SP  29

static uint32_t i_lui  (int rt, uint16_t imm)          { return (0x0Fu << 26) | ((uint32_t)rt << 16) | imm; }
static uint32_t i_ori  (int rt, int rs, uint16_t imm)   { return (0x0Du << 26) | ((uint32_t)rs << 21) | ((uint32_t)rt << 16) | imm; }
static uint32_t i_addiu(int rt, int rs, int16_t imm)    { return (0x09u << 26) | ((uint32_t)rs << 21) | ((uint32_t)rt << 16) | (uint16_t)imm; }
static uint32_t i_addu (int rd, int rs, int rt)         { return ((uint32_t)rs << 21) | ((uint32_t)rt << 16) | ((uint32_t)rd << 11) | 0x21u; }
static uint32_t i_sw   (int rt, int16_t off, int rs)    { return (0x2Bu << 26) | ((uint32_t)rs << 21) | ((uint32_t)rt << 16) | (uint16_t)off; }
static uint32_t i_lw   (int rt, int16_t off, int rs)    { return (0x23u << 26) | ((uint32_t)rs << 21) | ((uint32_t)rt << 16) | (uint16_t)off; }
static uint32_t i_nop  (void)                           { return 0u; }

// ── check bookkeeping: a plan up front, so a run that stops early cannot read as a pass ──────────
#define PLANNED_CHECKS 22
static int s_ran = 0, s_failed = 0;

static void check_u32(const char *what, uint32_t got, uint32_t want) {
  s_ran++;
  if (got == want) {
    printf("  ok   %-46s = 0x%08X\n", what, got);
  } else {
    s_failed++;
    printf("  FAIL %-46s = 0x%08X, expected 0x%08X\n", what, got, want);
  }
}
static void check_stop(const char *what, OracleStop got, OracleStop want) {
  s_ran++;
  if (got == want) {
    printf("  ok   %-46s = %s\n", what, oracle_stop_name(got));
  } else {
    s_failed++;
    printf("  FAIL %-46s = %s, expected %s\n", what, oracle_stop_name(got), oracle_stop_name(want));
  }
}

// The fixture's load address and entry. 0x80010000 is where a real PS-X EXE typically lands, chosen so
// the spike exercises the KSEG0 mirror rather than the physical alias — that mirror is the one every
// game's `t_addr` actually uses, so a FastMap bug there would be invisible under a KUSEG-only fixture.
#define FIX_ADDR 0x80010000u
#define FIX_GP   0x80011234u
#define FIX_SP   0x801FFF00u

// ── POSITIVE: a program whose every result is derived here, by hand ───────────────────────────────
//
//   lui   $t0, 0x1234        $t0 = 0x12340000
//   ori   $t0, $t0, 0x5678   $t0 = 0x12345678
//   addiu $t1, $zero, 100    $t1 = 0x00000064
//   addu  $t2, $t0, $t1      $t2 = 0x12345678 + 0x64 = 0x123456DC
//   sw    $t2, -16($sp)      [0x801FFEF0] = 0x123456DC
//   lw    $t3, -16($sp)      $t3 = 0x123456DC   (after its load-delay slot)
//   nop                      the load-delay slot — R3000A has no interlock, so $t3 is only
//                            architecturally readable after this instruction
//   nop ...                  and the rest of RAM is zeros, which decode as `sll $zero,$zero,0` = nop,
//                            so the CPU runs harmlessly forward until the cycle budget ends
// Built once, used by BOTH the bulk-run case and the single-step case, so "stepping agrees with running"
// is a statement about the same 8 instructions rather than about two fixtures that happen to look alike.
static int build_fixture(uint32_t *prog) {
  int n = 0;
  prog[n++] = i_lui  (R_T0, 0x1234);
  prog[n++] = i_ori  (R_T0, R_T0, 0x5678);
  prog[n++] = i_addiu(R_T1, R_ZERO, 100);
  prog[n++] = i_addu (R_T2, R_T0, R_T1);
  prog[n++] = i_sw   (R_T2, -16, R_SP);
  prog[n++] = i_lw   (R_T3, -16, R_SP);
  prog[n++] = i_nop();
  prog[n++] = i_nop();
  return n;
}

#define WINDOW_CYCLES 200

static int positive_case(void) {
  uint32_t prog[8];
  const int n = build_fixture(prog);

  if (!oracle_init())                                                        return 0;
  if (!oracle_load_exe(prog, (uint32_t)(n * 4), FIX_ADDR, FIX_ADDR, FIX_GP, FIX_SP)) return 0;

  const OracleStop stop = oracle_run(WINDOW_CYCLES);   // 8 real instructions then nops; no device touched

  OracleState st;
  oracle_capture(&st);

  check_stop("positive: window ended cleanly",   stop, ORACLE_STOP_BUDGET);
  check_u32 ("positive: $t0 = lui|ori",          st.gpr[R_T0], 0x12345678u);
  check_u32 ("positive: $t1 = addiu 100",        st.gpr[R_T1], 0x00000064u);
  check_u32 ("positive: $t2 = $t0 + $t1",        st.gpr[R_T2], 0x123456DCu);
  check_u32 ("positive: $t3 = lw of what sw wrote", st.gpr[R_T3], 0x123456DCu);
  check_u32 ("positive: $gp survived injection", st.gpr[28], FIX_GP);
  check_u32 ("positive: $sp survived injection", st.gpr[29], FIX_SP);

  // The store must be visible in RAM itself, not only in a register: that is the byte-compare surface
  // milestone 2 will diff, so it gets checked here rather than assumed.
  const uint32_t off = (FIX_SP - 16) & 0x1FFFFFFFu;
  uint32_t in_ram = 0;
  memcpy(&in_ram, oracle_main_ram() + off, 4);
  check_u32("positive: RAM at $sp-16 holds the store", in_ram, 0x123456DCu);

  // The PC must have moved FORWARD AND STILL BE IN RAM. "advanced past the entry" alone is not enough,
  // and that is measured rather than argued: with the FastMap deliberately left unpopulated in a
  // throwaway copy of the shim (2026-08-13), the CPU fetched zeros, took a bus error, and ended at
  // 0xBFC00180 — the exception vector — which a bare `pc > entry` test reported as ok while every
  // register check around it failed. A wild jump is not progress. So the bound is the mirror the fixture
  // was injected into: forward of the entry, and inside the 2 MB of main RAM behind it.
  s_ran++;
  const uint32_t ram_end = (FIX_ADDR & 0xFFE00000u) + 0x00200000u;
  if (st.pc > FIX_ADDR && st.pc < ram_end) {
    printf("  ok   %-46s = 0x%08X (advanced %u bytes, still in RAM)\n",
           "positive: PC advanced, and stayed in RAM", st.pc, st.pc - FIX_ADDR);
  } else {
    s_failed++;
    printf("  FAIL %-46s = 0x%08X — entry was 0x%08X, RAM ends 0x%08X. %s\n",
           "positive: PC advanced, and stayed in RAM", st.pc, FIX_ADDR, ram_end,
           st.pc <= FIX_ADDR ? "The CPU never fetched."
                             : "The CPU left RAM — a fault or a wild jump, not execution.");
  }

  oracle_teardown();
  return 1;
}

// ── NEGATIVE: the window must END, loudly, at a device access ─────────────────────────────────────
//
//   lui   $t0, 0x1F80        $t0 = 0x1F800000
//   ori   $t0, $t0, 0x1814   $t0 = 0x1F801814  — GPUSTAT, the GPU status register
//   lw    $t1, 0($t0)        a hardware read: main RAM and the scratchpad both refuse this
//
// 0x1F801814 is deliberately just past the scratchpad (0x1F800000..0x1F8003FF), so this also checks the
// scratchpad's UPPER bound rather than only that some far address is rejected.
static int negative_case(void) {
  uint32_t prog[4];
  int n = 0;
  prog[n++] = i_lui(R_T0, 0x1F80);
  prog[n++] = i_ori(R_T0, R_T0, 0x1814);
  prog[n++] = i_lw (R_T1, 0, R_T0);
  prog[n++] = i_nop();

  if (!oracle_init())                                                        return 0;
  if (!oracle_load_exe(prog, (uint32_t)(n * 4), FIX_ADDR, FIX_ADDR, FIX_GP, FIX_SP)) return 0;

  printf("  (the `oracle: HARDWARE READ32` block below is the EXPECTED output of this case)\n");
  const OracleStop stop = oracle_run(200);

  OracleState st;
  oracle_capture(&st);

  check_stop("negative: window ended at hardware", stop, ORACLE_STOP_HARDWARE);
  check_u32 ("negative: the offending address",    st.stop_addr, 0x1F801814u);
  check_u32 ("negative: $t0 computed the address", st.gpr[R_T0], 0x1F801814u);

  oracle_teardown();
  return 1;
}

// ── STEPPING: one instruction at a time must land where one bulk run lands ────────────────────────
//
// This is the property milestone 2 rests on. A register-level differential localises a divergence to ONE
// instruction, which means driving both sides a step at a time — and that is only meaningful if stepping
// and running are the same execution. The core keeps cycle-relative deadlines (`gte_ts_done`,
// `muldiv_ts_done`, the load-absorb counters) as absolute values against its own timestamp, so a stepper
// that restarted the clock at 0 each call would expire stalls early and quietly produce a DIFFERENT
// result from the bulk run. That is exactly what this compares, rather than assuming it away.
static int stepping_case(void) {
  uint32_t prog[8];
  const int n = build_fixture(prog);

  // Reference: the bulk run, captured.
  if (!oracle_init()) return 0;
  if (!oracle_load_exe(prog, (uint32_t)(n * 4), FIX_ADDR, FIX_ADDR, FIX_GP, FIX_SP)) return 0;
  oracle_run(WINDOW_CYCLES);
  OracleState bulk;
  oracle_capture(&bulk);
  oracle_teardown();

  // The same window, reached one step at a time.
  if (!oracle_init()) return 0;
  if (!oracle_load_exe(prog, (uint32_t)(n * 4), FIX_ADDR, FIX_ADDR, FIX_GP, FIX_SP)) return 0;

  int      steps    = 0;
  uint32_t distinct = 0, last_pc = 0;
  OracleStop st = ORACLE_STOP_NONE;
  while (oracle_timestamp() < WINDOW_CYCLES) {
    st = oracle_step();
    if (st != ORACLE_STOP_BUDGET) break;   // hardware or an event ended the window; not a step failure
    steps++;
    OracleState now;
    oracle_capture(&now);
    if (now.pc != last_pc) { distinct++; last_pc = now.pc; }
    if (steps > WINDOW_CYCLES * 4) break;  // a stepper that never advances the clock must not spin forever
  }
  OracleState stepped;
  oracle_capture(&stepped);

  check_u32("stepping: $t0 matches the bulk run", stepped.gpr[R_T0], bulk.gpr[R_T0]);
  check_u32("stepping: $t2 matches the bulk run", stepped.gpr[R_T2], bulk.gpr[R_T2]);
  check_u32("stepping: $t3 matches the bulk run", stepped.gpr[R_T3], bulk.gpr[R_T3]);
  check_u32("stepping: PC matches the bulk run",  stepped.pc,        bulk.pc);

  // Without these two, every check above would pass on a stepper that executed nothing at all: the
  // captures would agree because both sides would be at the entry with zeroed registers. A comparison
  // that agrees because neither side moved is the failure mode this whole file exists to refuse.
  s_ran++;
  if (steps > 1) {
    printf("  ok   %-46s = %d steps, %u distinct PC(s)\n",
           "stepping: it really stepped, repeatedly", steps, distinct);
  } else {
    s_failed++;
    printf("  FAIL %-46s = %d step(s) — the stepper does not advance\n",
           "stepping: it really stepped, repeatedly", steps);
  }
  s_ran++;
  if (distinct > 8) {
    printf("  ok   %-46s = %u distinct PC(s) over %d step(s)\n",
           "stepping: PC moved through the program", distinct, steps);
  } else {
    s_failed++;
    printf("  FAIL %-46s = %u distinct PC(s) — the fixture is 8 instructions, so a real trace must\n"
           "       visit more than 8 addresses before the window ends\n",
           "stepping: PC moved through the program", distinct);
  }

  oracle_teardown();
  return 1;
}

// ── MIRRORING: main RAM appears FOUR TIMES across the 8 MB window ─────────────────────────────────
//
// The reference decodes main RAM as `A < 0x00800000` with the offset masked to `A & 0x1FFFFF`
// (libretro.c:1085-1108), so the 2 MB of physical RAM is mirrored four times. This is not academic:
// MEASURED 2026-08-13, Spider-Man's crt0 sets `sp = 0x807FFFF8`, because its stack-top global holds
// 0x00800000. That address is the top of RAM through the fourth mirror and is entirely legitimate — and
// this shim originally rejected it as a hardware access, which would have ended Spider-Man's window at a
// boundary that does not exist, while instruction FETCH worked because the FastMap already mirrored 4x.
//
//   lui   $t0, 0x8080        $t0 = 0x80800000
//   addiu $t0, $t0, -8       $t0 = 0x807FFFF8   (Spider-Man's actual sp)
//   lui   $t1, 0xABCD
//   ori   $t1, $t1, 0x1234   $t1 = 0xABCD1234
//   sw    $t1, 0($t0)        a store through the FOURTH mirror
//   lw    $t2, 0($sp-ish)    ... read back through the FIRST, at 0x801FFFF8
static int mirroring_case(void) {
  const uint32_t kMirrorAddr = 0x807FFFF8u;   // 4th mirror, top of RAM
  const uint32_t kFirstAddr  = 0x801FFFF8u;   // the same physical word via the 1st mirror
  uint32_t prog[8];
  int n = 0;
  prog[n++] = i_lui  (R_T0, 0x8080);
  prog[n++] = i_addiu(R_T0, R_T0, -8);
  prog[n++] = i_lui  (R_T1, 0xABCD);
  prog[n++] = i_ori  (R_T1, R_T1, 0x1234);
  prog[n++] = i_sw   (R_T1, 0, R_T0);
  prog[n++] = i_lw   (R_T2, 0, R_T0);
  prog[n++] = i_nop();
  prog[n++] = i_nop();

  if (!oracle_init()) return 0;
  if (!oracle_load_exe(prog, (uint32_t)(n * 4), FIX_ADDR, FIX_ADDR, FIX_GP, FIX_SP)) return 0;

  const OracleStop stop = oracle_run(200);
  OracleState st;
  oracle_capture(&st);

  // A clean stop is itself the check: without the mirror this run ends ORACLE_STOP_HARDWARE at 0x807FFFF8.
  check_stop("mirroring: high address is RAM, not hardware", stop, ORACLE_STOP_BUDGET);
  check_u32 ("mirroring: $t0 built Spider-Man's real sp",    st.gpr[R_T0], kMirrorAddr);
  check_u32 ("mirroring: read back through the 4th mirror",  st.gpr[R_T2], 0xABCD1234u);

  // And the physical word must be the SAME one the first mirror names — that is what "mirror" means, and
  // checking only the read-back would pass on a shim that gave 0x807FFFF8 its own separate storage.
  uint32_t via_first = 0;
  memcpy(&via_first, oracle_main_ram() + (kFirstAddr & 0x1FFFFFu), 4);
  check_u32("mirroring: same physical word via 0x801FFFF8", via_first, 0xABCD1234u);

  oracle_teardown();
  return 1;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);   // unbuffered: a crash mid-run must not swallow the checks already
  setvbuf(stderr, NULL, _IONBF, 0);   // printed, and stderr/stdout must interleave in the real order

  printf("psxport oracle spike — milestone 1 of docs/plans/oracle-against-beetle.md\n");
  printf("PLAN: %d checks across 2 program classes.\n", PLANNED_CHECKS);
  printf("  POSITIVE (9 checks): inject 8 hand-assembled instructions at 0x%08X, run 200 cycles,\n"
         "    assert $t0-$t3, $gp, $sp, the stored word in RAM, a clean stop, and that PC advanced within RAM.\n", FIX_ADDR);
  printf("  NEGATIVE (3 checks): read GPUSTAT at 0x1F801814 and assert the run REPORTS a hardware\n"
         "    stop at that address, rather than reading 0 and continuing.\n");
  printf("  STEPPING (6 checks): run the SAME fixture one instruction at a time and require it to land\n"
         "    exactly where the bulk run landed, having actually stepped through the program.\n");
  printf("  MIRRORING (4 checks): store through 0x807FFFF8 — the address Spider-Man's crt0 really uses as\n"
         "    its stack pointer — and require it to be RAM via the 4th mirror, not a hardware access.\n");
  printf("  BLIND SPOTS, stated so this is not mistaken for more than it is: no BIOS is mapped, no\n"
         "    real game executable is loaded, no comparison against psxport's own paths is performed,\n"
         "    and nothing here exercises the GTE, DMA, CD or timers.\n\n");

  printf("POSITIVE — a program whose results are derived by hand in this file:\n");
  if (!positive_case()) { printf("  REFUSED: oracle setup failed; the positive case did not run.\n"); return 2; }

  printf("\nNEGATIVE — a hardware access must end the window and be named:\n");
  if (!negative_case()) { printf("  REFUSED: oracle setup failed; the negative case did not run.\n"); return 2; }

  printf("\nSTEPPING — one instruction at a time must equal one bulk run:\n");
  if (!stepping_case()) { printf("  REFUSED: oracle setup failed; the stepping case did not run.\n"); return 2; }

  printf("\nMIRRORING — 2 MB of RAM appears four times across the 8 MB window:\n");
  if (!mirroring_case()) { printf("  REFUSED: oracle setup failed; the mirroring case did not run.\n"); return 2; }

  printf("\n%d checks planned, %d ran, %d failed.\n", PLANNED_CHECKS, s_ran, s_failed);
  if (s_ran != PLANNED_CHECKS) {
    printf("REFUSING to report a result: %d checks ran but %d were planned, so this run does not cover\n"
           "what it claims to. Fix the plan or the code path that skipped a check.\n", s_ran, PLANNED_CHECKS);
    return 2;
  }
  if (s_failed) {
    printf("FAILED. The oracle does not execute injected code correctly; do not build a compare on it.\n");
    return 1;
  }
  printf("PASSED. The vendored Mednafen CPU steps injected MIPS without libretro.c, and reports a\n"
         "hardware access instead of hiding it. This says NOTHING about the port yet — milestone 2.\n");
  return 0;
}

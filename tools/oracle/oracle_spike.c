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

#include "cpu.h"
#include "dma_dpcr.h"
#include "oracle_shim.h"
#include "psx.h"

// ── MIPS R3000A encoders, so the fixture reads as instructions rather than hex ────────────────────
#define R_ZERO 0
#define R_V0 2
#define R_V1 3
#define R_A0 4
#define R_A1 5
#define R_T0 8
#define R_T1 9
#define R_T2 10
#define R_T3 11
#define R_SP 29
#define R_RA 31

static uint32_t i_lui(int rt, uint16_t imm) {
  return (0x0Fu << 26) | ((uint32_t)rt << 16) | imm;
}
static uint32_t i_ori(int rt, int rs, uint16_t imm) {
  return (0x0Du << 26) | ((uint32_t)rs << 21) | ((uint32_t)rt << 16) | imm;
}
static uint32_t i_addiu(int rt, int rs, int16_t imm) {
  return (0x09u << 26) | ((uint32_t)rs << 21) | ((uint32_t)rt << 16) | (uint16_t)imm;
}
static uint32_t i_addu(int rd, int rs, int rt) {
  return ((uint32_t)rs << 21) | ((uint32_t)rt << 16) | ((uint32_t)rd << 11) | 0x21u;
}
static uint32_t i_sw(int rt, int16_t off, int rs) {
  return (0x2Bu << 26) | ((uint32_t)rs << 21) | ((uint32_t)rt << 16) | (uint16_t)off;
}
static uint32_t i_lw(int rt, int16_t off, int rs) {
  return (0x23u << 26) | ((uint32_t)rs << 21) | ((uint32_t)rt << 16) | (uint16_t)off;
}
static uint32_t i_lhu(int rt, int16_t off, int rs) {
  return (0x25u << 26) | ((uint32_t)rs << 21) | ((uint32_t)rt << 16) | (uint16_t)off;
}
static uint32_t i_sh(int rt, int16_t off, int rs) {
  return (0x29u << 26) | ((uint32_t)rs << 21) | ((uint32_t)rt << 16) | (uint16_t)off;
}
static uint32_t i_nop(void) {
  return 0u;
}
static uint32_t i_jal(uint32_t target) {
  return (0x03u << 26) | ((target >> 2) & 0x03FFFFFFu);
}
static uint32_t i_jr(int rs) {
  return ((uint32_t)rs << 21) | 0x08u;
}
static uint32_t i_syscall(void) {
  return 0x0000000Cu;
}

// ── check bookkeeping: a plan up front, so a run that stops early cannot read as a pass ──────────
#define PLANNED_CHECKS 84
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
#define FIX_GP 0x80011234u
#define FIX_SP 0x801FFF00u

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
  prog[n++] = i_lui(R_T0, 0x1234);
  prog[n++] = i_ori(R_T0, R_T0, 0x5678);
  prog[n++] = i_addiu(R_T1, R_ZERO, 100);
  prog[n++] = i_addu(R_T2, R_T0, R_T1);
  prog[n++] = i_sw(R_T2, -16, R_SP);
  prog[n++] = i_lw(R_T3, -16, R_SP);
  prog[n++] = i_nop();
  prog[n++] = i_nop();
  return n;
}

#define WINDOW_CYCLES 200

static int positive_case(void) {
  uint32_t prog[8];
  const int n = build_fixture(prog);

  if (!oracle_init()) {
    return 0;
  }
  if (!oracle_load_exe(prog, (uint32_t)(n * 4), FIX_ADDR, FIX_ADDR, FIX_GP, FIX_SP)) {
    return 0;
  }

  const OracleStop stop = oracle_run(WINDOW_CYCLES); // 8 real instructions then nops; no device touched

  OracleState st;
  oracle_capture(&st);

  check_stop("positive: window ended cleanly", stop, ORACLE_STOP_BUDGET);
  check_u32("positive: $t0 = lui|ori", st.gpr[R_T0], 0x12345678u);
  check_u32("positive: $t1 = addiu 100", st.gpr[R_T1], 0x00000064u);
  check_u32("positive: $t2 = $t0 + $t1", st.gpr[R_T2], 0x123456DCu);
  check_u32("positive: $t3 = lw of what sw wrote", st.gpr[R_T3], 0x123456DCu);
  check_u32("positive: $gp survived injection", st.gpr[28], FIX_GP);
  check_u32("positive: $sp survived injection", st.gpr[29], FIX_SP);

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
           "positive: PC advanced, and stayed in RAM",
           st.pc,
           st.pc - FIX_ADDR);
  } else {
    s_failed++;
    printf("  FAIL %-46s = 0x%08X — entry was 0x%08X, RAM ends 0x%08X. %s\n",
           "positive: PC advanced, and stayed in RAM",
           st.pc,
           FIX_ADDR,
           ram_end,
           st.pc <= FIX_ADDR ? "The CPU never fetched." : "The CPU left RAM — a fault or a wild jump, not execution.");
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
  prog[n++] = i_lw(R_T1, 0, R_T0);
  prog[n++] = i_nop();

  if (!oracle_init()) {
    return 0;
  }
  if (!oracle_load_exe(prog, (uint32_t)(n * 4), FIX_ADDR, FIX_ADDR, FIX_GP, FIX_SP)) {
    return 0;
  }

  printf("  (the `oracle: UNSUPPORTED HARDWARE READ32` block below is expected here)\n");
  const OracleStop stop = oracle_run(200);

  OracleState st;
  oracle_capture(&st);

  check_stop("negative: window ended at hardware", stop, ORACLE_STOP_HARDWARE);
  check_u32("negative: the offending address", st.stop_addr, 0x1F801814u);
  check_u32("negative: $t0 computed the address", st.gpr[R_T0], 0x1F801814u);

  oracle_teardown();
  return 1;
}

// ── IRQ BUS: real I_MASK/I_STAT accesses must remain on this same CPU ─────────────────────────────
//
//   lui   $t0, 0x1F80
//   ori   $t0, $t0, 0x1074  $t0 = I_MASK
//   addiu $t1, $zero, 0x3333
//   sh    $t1, 0($t0)       I_MASK = 0x3333
//   lhu   $t2, 0($t0)       $t2 = the real controller's mask readback after its load delay
//   nop
//   sh    $zero, -4($t0)    acknowledge no asserted I_STAT bits
//   addiu $t3, $zero, 0x4444 proves execution continued beyond the complete sequence
//
// This is the same access-width and write/read/write shape Tekken reaches. It uses no title address or
// expected PC: I_STAT/I_MASK are hardware facts, and the negative GPUSTAT case above must remain the
// opposite answer so adding one modeled device cannot silently turn every unknown register into zero.
static int irq_bus_case(void) {
  uint32_t prog[9];
  int n = 0;
  prog[n++] = i_lui(R_T0, 0x1F80);
  prog[n++] = i_ori(R_T0, R_T0, 0x1074);
  prog[n++] = i_addiu(R_T1, R_ZERO, 0x3333);
  prog[n++] = i_sh(R_T1, 0, R_T0);
  prog[n++] = i_lhu(R_T2, 0, R_T0);
  prog[n++] = i_nop();
  prog[n++] = i_sh(R_ZERO, -4, R_T0);
  prog[n++] = i_addiu(R_T3, R_ZERO, 0x4444);
  prog[n++] = i_nop();

  if (!oracle_init()) {
    return 0;
  }
  if (!oracle_load_exe(prog, (uint32_t)(n * 4), FIX_ADDR, FIX_ADDR, FIX_GP, FIX_SP)) {
    return 0;
  }

  const OracleStop stop = oracle_run(200);
  OracleState st;
  oracle_capture(&st);
  check_stop("IRQ bus: sequence stayed on the same CPU", stop, ORACLE_STOP_BUDGET);
  check_u32("IRQ bus: exact I_MASK address", st.gpr[R_T0], 0x1F801074u);
  check_u32("IRQ bus: controller supplied mask readback", st.gpr[R_T2], 0x00003333u);
  check_u32("IRQ bus: caller continued after I_STAT write", st.gpr[R_T3], 0x00004444u);

  oracle_teardown();
  return 1;
}

// ── DMA BUS: DPCR must use the vendored controller without claiming every device register ─────────
//
//   lui  $t0, 0x1F80
//   ori  $t0, $t0, 0x10F0  $t0 = DPCR
//   lui  $t1, 0x3333
//   ori  $t1, $t1, 0x3333
//   sw   $t1, 0($t0)       DPCR = 0x33333333
//   lw   $t2, 0($t0)       $t2 = the real controller's readback after its load delay
//   nop
//   addiu $t3, $zero, 0x4444 proves execution continued beyond the complete sequence
//
// The address and value are generic controller facts, not a title PC or serial. The adjacent DICR
// remains the unsupported opposite answer, so adding DPCR cannot silently absorb the rest of DMA.
static int dma_bus_case(void) {
  uint32_t prog[9];
  int n = 0;
  prog[n++] = i_lui(R_T0, 0x1F80);
  prog[n++] = i_ori(R_T0, R_T0, 0x10F0);
  prog[n++] = i_lui(R_T1, 0x3333);
  prog[n++] = i_ori(R_T1, R_T1, 0x3333);
  prog[n++] = i_sw(R_T1, 0, R_T0);
  prog[n++] = i_lw(R_T2, 0, R_T0);
  prog[n++] = i_nop();
  prog[n++] = i_addiu(R_T3, R_ZERO, 0x4444);
  prog[n++] = i_nop();

  if (!oracle_init()) {
    return 0;
  }
  uint32_t reset_dpcr = 0;
  const int reset_owned = DMA_DPCR_Read(0x1F8010F0u, &reset_dpcr);
  check_u32("DMA bus: DPCR reset address is owned", (uint32_t)reset_owned, 1u);
  check_u32("DMA bus: DPCR uses the hardware reset value", reset_dpcr, DMA_DPCR_RESET);

  DMA_DPCR_Write(0x1F8010F1u, 0xAABBCCDDu);
  check_u32("DMA bus: byte-lane 1 shifts the full source", DMA_DPCR_SaveStateValue(), 0xBBCCDD00u);
  DMA_DPCR_Write(0x1F8010F2u, 0xAABBCCDDu);
  check_u32("DMA bus: byte-lane 2 shifts the full source", DMA_DPCR_SaveStateValue(), 0xCCDD0000u);
  DMA_DPCR_Power();
  if (!oracle_load_exe(prog, (uint32_t)(n * 4), FIX_ADDR, FIX_ADDR, FIX_GP, FIX_SP)) {
    return 0;
  }

  const OracleStop stop = oracle_run(200);
  OracleState st;
  oracle_capture(&st);
  check_stop("DMA bus: sequence stayed on the same CPU", stop, ORACLE_STOP_BUDGET);
  check_u32("DMA bus: exact DPCR address", st.gpr[R_T0], 0x1F8010F0u);
  check_u32("DMA bus: controller supplied DPCR readback", st.gpr[R_T2], 0x33333333u);
  check_u32("DMA bus: caller continued after DPCR read", st.gpr[R_T3], 0x00004444u);

  oracle_teardown();

  uint32_t unowned[7];
  n = 0;
  unowned[n++] = i_lui(R_T0, 0x1F80);
  unowned[n++] = i_ori(R_T0, R_T0, 0x10F4);
  unowned[n++] = i_lui(R_T1, 0x5555);
  unowned[n++] = i_ori(R_T1, R_T1, 0x5555);
  unowned[n++] = i_sw(R_T1, 0, R_T0);
  unowned[n++] = i_addiu(R_T3, R_ZERO, 0x6666);
  unowned[n++] = i_nop();

  if (!oracle_init()) {
    return 0;
  }
  if (!oracle_load_exe(unowned, (uint32_t)(n * 4), FIX_ADDR, FIX_ADDR, FIX_GP, FIX_SP)) {
    return 0;
  }

  printf("  (the `oracle: UNSUPPORTED HARDWARE WRITE32` block below is expected for DICR)\n");
  const OracleStop unowned_stop = oracle_run(200);
  oracle_capture(&st);
  check_stop("DMA bus: unowned DICR ended the window", unowned_stop, ORACLE_STOP_HARDWARE);
  check_u32("DMA bus: DICR was the offending address", st.stop_addr, 0x1F8010F4u);
  check_u32("DMA bus: execution did not cross unowned DICR", st.gpr[R_T3], 0u);
  OracleDeviceState refused_devices;
  check_u32("DMA bus: tainted device snapshot is refused", (uint32_t)oracle_capture_devices(&refused_devices), 0u);
  check_stop("DMA bus: a later step remains tainted", oracle_step(), ORACLE_STOP_HARDWARE);

  oracle_teardown();
  return 1;
}

// ─── DEVICE BOUNDARY: capture only after the exact generic IRQ/DPCR write sequence ───────────────
//
// This is the architectural shape a consumer needs: I_MASK write/read, I_STAT write, DPCR write,
// then a real jal whose delay slot has executed while the callee has not. The snapshot is deliberately
// separate from OracleState, because CPU equality must not imply device equality.
static int device_boundary_case(void) {
  enum { TARGET_INDEX = 16 };
  const uint32_t target = FIX_ADDR + TARGET_INDEX * 4u;
  uint32_t prog[20] = {0};
  int n = 0;
  prog[n++] = i_lui(R_T0, 0x1F80);
  prog[n++] = i_ori(R_T0, R_T0, 0x1074);
  prog[n++] = i_sh(R_ZERO, 0, R_T0);
  prog[n++] = i_lhu(R_T1, 0, R_T0);
  prog[n++] = i_nop();
  prog[n++] = i_sh(R_ZERO, -4, R_T0);
  prog[n++] = i_lui(R_T2, 0x3333);
  prog[n++] = i_ori(R_T2, R_T2, 0x3333);
  prog[n++] = i_sw(R_T2, 0x7C, R_T0);
  prog[n++] = i_jal(target);
  prog[n++] = i_addiu(R_A1, R_ZERO, 0x041A);
  prog[TARGET_INDEX] = i_addiu(R_T3, R_ZERO, 0x7777);

  if (!oracle_init()) {
    return 0;
  }
  if (!oracle_load_exe(prog, sizeof(prog), FIX_ADDR, FIX_ADDR, FIX_GP, FIX_SP)) {
    return 0;
  }

  OracleState state;
  int steps = 0;
  do {
    oracle_step();
    oracle_capture(&state);
    steps++;
  } while (state.pc != target && steps < 24);

  OracleDeviceState devices;
  const int captured = oracle_capture_devices(&devices);
  check_u32("device boundary: reached the jal target", state.pc, target);
  check_u32("device boundary: jal delay slot executed", state.gpr[R_A1], 0x0000041Au);
  check_u32("device boundary: callee has not executed", state.gpr[R_T3], 0u);
  check_u32("device boundary: clean device state captured", (uint32_t)captured, 1u);
  check_u32("device boundary: every register is valid", devices.valid, ORACLE_DEVICE_ALL);
  check_u32("device boundary: every register was written", devices.writes, ORACLE_DEVICE_ALL);
  check_u32("device boundary: I_STAT is masked to 11 bits", devices.i_stat, 0u);
  check_u32("device boundary: I_MASK is masked to 11 bits", devices.i_mask, 0u);
  check_u32("device boundary: DPCR kept all 32 bits", devices.dpcr, 0x33333333u);

  oracle_teardown();
  return 1;
}

// ─── EVENT TAINT: a dropped scheduler request can never become clean device evidence ────────────────
static int event_taint_case(void) {
  const uint32_t prog[] = {0};
  if (!oracle_init()) {
    return 0;
  }
  if (!oracle_load_exe(prog, sizeof(prog), FIX_ADDR, FIX_ADDR, FIX_GP, FIX_SP)) {
    return 0;
  }

  PSX_SetEventNT(0, 1);
  OracleDeviceState devices;
  check_u32("event taint: device snapshot is refused", (uint32_t)oracle_capture_devices(&devices), 0u);
  check_stop("event taint: a later step remains tainted", oracle_step(), ORACLE_STOP_EVENT);

  oracle_teardown();
  return 1;
}

// ─── SYSCALL RETURN: validate the CPU-produced CP0 exception before resuming ────────────────────────
static int syscall_return_case(void) {
  uint32_t prog[8] = {0};
  prog[0] = i_addiu(R_A0, R_ZERO, 1);
  prog[1] = i_syscall();
  prog[2] = i_addiu(R_T0, R_ZERO, 0x1234);

  if (!oracle_init()) {
    return 0;
  }
  if (!oracle_load_exe(prog, sizeof(prog), FIX_ADDR, FIX_ADDR, FIX_GP, FIX_SP)) {
    return 0;
  }

  // Exercise nonzero mode-stack bits so exception push and RFE pop cannot both pass by preserving zero.
  CPU_SetCOP0(12u, CPU_GetCOP0(12u) | 0x3u);
  OracleState before_exception;
  oracle_capture(&before_exception);
  OracleState exception;
  int steps = 0;
  do {
    oracle_step();
    oracle_capture(&exception);
    steps++;
  } while (exception.pc != 0xBFC00180u && steps < 8);

  check_u32("syscall return: CPU reached the BEV vector", exception.pc, 0xBFC00180u);
  check_u32("syscall return: CP0 ExcCode is syscall", (exception.cp0_cause >> 2) & 0x1Fu, 8u);
  check_u32("syscall return: exception is not in a delay slot", exception.cp0_cause >> 31, 0u);
  check_u32("syscall return: CP0 EPC names the syscall", exception.cp0_epc, FIX_ADDR + 4u);
  check_u32("syscall return: exception pushed the SR mode stack",
            exception.cp0_status,
            (before_exception.cp0_status & ~0x3Fu) | ((before_exception.cp0_status << 2) & 0x3Fu));

  const int wrong_selector = oracle_resume_syscall_return(2u, 0x11111111u, 0x22222222u);
  OracleState refused;
  oracle_capture(&refused);
  check_u32("syscall return: wrong selector is refused", (uint32_t)wrong_selector, 0u);
  check_u32("syscall return: refusal preserves vector PC", refused.pc, exception.pc);

  CPU_SetCOP0(13u, exception.cp0_cause | 0x80000000u);
  const int delayed_exception = oracle_resume_syscall_return(1u, 0x11111111u, 0x22222222u);
  check_u32("syscall return: delay-slot exception is refused", (uint32_t)delayed_exception, 0u);
  CPU_SetCOP0(13u, exception.cp0_cause);

  const int resumed = oracle_resume_syscall_return(1u, 0x11111111u, 0x22222222u);
  OracleState after_resume;
  oracle_capture(&after_resume);
  check_u32("syscall return: exact boundary is accepted", (uint32_t)resumed, 1u);
  check_u32("syscall return: resumes after syscall", after_resume.pc, FIX_ADDR + 8u);
  check_u32("syscall return: next PC is sequential", after_resume.next_pc, FIX_ADDR + 12u);
  check_u32("syscall return: explicit $v0 is installed", after_resume.gpr[R_V0], 0x11111111u);
  check_u32("syscall return: explicit $v1 is installed", after_resume.gpr[R_V1], 0x22222222u);
  check_u32("syscall return: RFE pops the SR mode stack once",
            after_resume.cp0_status,
            (exception.cp0_status & ~0x0Fu) | ((exception.cp0_status >> 2) & 0x0Fu));
  check_u32("syscall return: Cause is preserved", after_resume.cp0_cause, exception.cp0_cause);
  check_u32("syscall return: EPC is preserved", after_resume.cp0_epc, exception.cp0_epc);

  oracle_step();
  OracleState continued;
  oracle_capture(&continued);
  check_u32("syscall return: caller executes after resume", continued.gpr[R_T0], 0x00001234u);

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
  if (!oracle_init()) {
    return 0;
  }
  if (!oracle_load_exe(prog, (uint32_t)(n * 4), FIX_ADDR, FIX_ADDR, FIX_GP, FIX_SP)) {
    return 0;
  }
  oracle_run(WINDOW_CYCLES);
  OracleState bulk;
  oracle_capture(&bulk);
  oracle_teardown();

  // The same window, reached one step at a time.
  if (!oracle_init()) {
    return 0;
  }
  if (!oracle_load_exe(prog, (uint32_t)(n * 4), FIX_ADDR, FIX_ADDR, FIX_GP, FIX_SP)) {
    return 0;
  }

  int steps = 0;
  uint32_t distinct = 0, last_pc = 0;
  OracleStop st = ORACLE_STOP_NONE;
  while (oracle_timestamp() < WINDOW_CYCLES) {
    st = oracle_step();
    if (st != ORACLE_STOP_BUDGET) {
      break; // hardware or an event ended the window; not a step failure
    }
    steps++;
    OracleState now;
    oracle_capture(&now);
    if (now.pc != last_pc) {
      distinct++;
      last_pc = now.pc;
    }
    if (steps > WINDOW_CYCLES * 4) {
      break; // a stepper that never advances the clock must not spin forever
    }
  }
  OracleState stepped;
  oracle_capture(&stepped);

  check_u32("stepping: $t0 matches the bulk run", stepped.gpr[R_T0], bulk.gpr[R_T0]);
  check_u32("stepping: $t2 matches the bulk run", stepped.gpr[R_T2], bulk.gpr[R_T2]);
  check_u32("stepping: $t3 matches the bulk run", stepped.gpr[R_T3], bulk.gpr[R_T3]);
  check_u32("stepping: PC matches the bulk run", stepped.pc, bulk.pc);

  // Without these two, every check above would pass on a stepper that executed nothing at all: the
  // captures would agree because both sides would be at the entry with zeroed registers. A comparison
  // that agrees because neither side moved is the failure mode this whole file exists to refuse.
  s_ran++;
  if (steps > 1) {
    printf("  ok   %-46s = %d steps, %u distinct PC(s)\n", "stepping: it really stepped, repeatedly", steps, distinct);
  } else {
    s_failed++;
    printf(
        "  FAIL %-46s = %d step(s) — the stepper does not advance\n", "stepping: it really stepped, repeatedly", steps);
  }
  s_ran++;
  if (distinct > 8) {
    printf("  ok   %-46s = %u distinct PC(s) over %d step(s)\n",
           "stepping: PC moved through the program",
           distinct,
           steps);
  } else {
    s_failed++;
    printf("  FAIL %-46s = %u distinct PC(s) — the fixture is 8 instructions, so a real trace must\n"
           "       visit more than 8 addresses before the window ends\n",
           "stepping: PC moved through the program",
           distinct);
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
  const uint32_t kMirrorAddr = 0x807FFFF8u; // 4th mirror, top of RAM
  const uint32_t kFirstAddr = 0x801FFFF8u;  // the same physical word via the 1st mirror
  uint32_t prog[8];
  int n = 0;
  prog[n++] = i_lui(R_T0, 0x8080);
  prog[n++] = i_addiu(R_T0, R_T0, -8);
  prog[n++] = i_lui(R_T1, 0xABCD);
  prog[n++] = i_ori(R_T1, R_T1, 0x1234);
  prog[n++] = i_sw(R_T1, 0, R_T0);
  prog[n++] = i_lw(R_T2, 0, R_T0);
  prog[n++] = i_nop();
  prog[n++] = i_nop();

  if (!oracle_init()) {
    return 0;
  }
  if (!oracle_load_exe(prog, (uint32_t)(n * 4), FIX_ADDR, FIX_ADDR, FIX_GP, FIX_SP)) {
    return 0;
  }

  const OracleStop stop = oracle_run(200);
  OracleState st;
  oracle_capture(&st);

  // A clean stop is itself the check: without the mirror this run ends ORACLE_STOP_HARDWARE at 0x807FFFF8.
  check_stop("mirroring: high address is RAM, not hardware", stop, ORACLE_STOP_BUDGET);
  check_u32("mirroring: $t0 built Spider-Man's real sp", st.gpr[R_T0], kMirrorAddr);
  check_u32("mirroring: read back through the 4th mirror", st.gpr[R_T2], 0xABCD1234u);

  // And the physical word must be the SAME one the first mirror names — that is what "mirror" means, and
  // checking only the read-back would pass on a shim that gave 0x807FFFF8 its own separate storage.
  uint32_t via_first = 0;
  memcpy(&via_first, oracle_main_ram() + (kFirstAddr & 0x1FFFFFu), 4);
  check_u32("mirroring: same physical word via 0x801FFFF8", via_first, 0xABCD1234u);

  oracle_teardown();
  return 1;
}

// ── MODELED CALL RETURN: resume the same independent CPU after an explicitly owned external leaf ──
//
// The oracle intentionally maps no BIOS. A differential can still continue after a BIOS/libc leaf when
// the CONSUMER owns that leaf's semantics, but only if the continuation surface refuses a stale or wrong
// boundary. This fixture reaches physical 0xA0 through a real jal + three-instruction thunk, first asks
// to return from the wrong target (must refuse without mutation), then supplies explicit v0/v1 and proves
// that execution resumes at the captured $ra with the CPU timestamp and unrelated registers preserved.
static int modeled_call_return_case(void) {
  enum { THUNK_INDEX = 8 };
  const uint32_t return_pc = FIX_ADDR + 8u;
  const uint32_t thunk = FIX_ADDR + THUNK_INDEX * 4u;
  uint32_t prog[12] = {0};
  prog[0] = i_jal(thunk);
  prog[1] = i_addiu(R_T0, R_ZERO, 0x1111); // jal delay slot
  prog[2] = i_addiu(R_T3, R_ZERO, 0x2222); // first instruction after modeled return
  prog[THUNK_INDEX + 0] = i_addiu(R_T2, R_ZERO, 0xA0);
  prog[THUNK_INDEX + 1] = i_jr(R_T2);
  prog[THUNK_INDEX + 2] = i_addiu(R_T1, R_ZERO, 0x39); // jr delay slot: A(39h)

  if (!oracle_init()) {
    return 0;
  }
  if (!oracle_load_exe(prog, sizeof(prog), FIX_ADDR, FIX_ADDR, FIX_GP, FIX_SP)) {
    return 0;
  }

  OracleState at_target;
  int steps = 0;
  do {
    oracle_step();
    oracle_capture(&at_target);
    steps++;
  } while (at_target.pc != 0xA0u && steps < 16);

  check_u32("modeled return: reached external target", at_target.pc, 0xA0u);
  check_u32("modeled return: captured jal return PC", at_target.gpr[R_RA], return_pc);
  check_u32("modeled return: thunk selected A(39h)", at_target.gpr[R_T1], 0x39u);

  const int wrong_target = oracle_resume_call_return(0xB0u, return_pc, 0x12345678u, 0x89ABCDEFu);
  OracleState after_refusal;
  oracle_capture(&after_refusal);
  check_u32("modeled return: wrong target is refused", (uint32_t)wrong_target, 0u);
  check_u32("modeled return: refusal preserves PC", after_refusal.pc, at_target.pc);

  const int wrong_ra = oracle_resume_call_return(0xA0u, return_pc + 4u, 0x12345678u, 0x89ABCDEFu);
  OracleState after_ra_refusal;
  oracle_capture(&after_ra_refusal);
  check_u32("modeled return: wrong return PC is refused", (uint32_t)wrong_ra, 0u);
  check_u32("modeled return: return-PC refusal preserves PC", after_ra_refusal.pc, at_target.pc);

  const int resumed = oracle_resume_call_return(0xA0u, return_pc, 0x12345678u, 0x89ABCDEFu);
  OracleState after_resume;
  oracle_capture(&after_resume);
  check_u32("modeled return: exact boundary is accepted", (uint32_t)resumed, 1u);
  check_u32("modeled return: PC resumes at captured $ra", after_resume.pc, return_pc);
  check_u32("modeled return: successor is sequential", after_resume.next_pc, return_pc + 4u);
  check_u32("modeled return: explicit $v0 is installed", after_resume.gpr[R_V0], 0x12345678u);
  check_u32("modeled return: explicit $v1 is installed", after_resume.gpr[R_V1], 0x89ABCDEFu);
  check_u32("modeled return: unrelated register is preserved", after_resume.gpr[R_T0], at_target.gpr[R_T0]);
  check_u32("modeled return: timestamp is preserved", (uint32_t)after_resume.timestamp, (uint32_t)at_target.timestamp);

  oracle_step();
  OracleState continued;
  oracle_capture(&continued);
  check_u32("modeled return: caller executes after resume", continued.gpr[R_T3], 0x2222u);

  oracle_teardown();

  uint32_t pending_prog[13] = {0};
  pending_prog[0] = i_jal(thunk);
  pending_prog[1] = i_nop();
  pending_prog[THUNK_INDEX + 0] = i_addiu(R_T2, R_ZERO, 0xA0);
  pending_prog[THUNK_INDEX + 1] = i_addiu(R_T1, R_ZERO, 0x39);
  pending_prog[THUNK_INDEX + 2] = i_jr(R_T2);
  pending_prog[THUNK_INDEX + 3] = i_lw(R_T3, 0, R_SP);

  if (!oracle_init()) {
    return 0;
  }
  if (!oracle_load_exe(pending_prog, sizeof(pending_prog), FIX_ADDR, FIX_ADDR, FIX_GP, FIX_SP)) {
    return 0;
  }
  steps = 0;
  do {
    oracle_step();
    oracle_capture(&at_target);
    steps++;
  } while (at_target.pc != 0xA0u && steps < 16);
  const int pending_load = oracle_resume_call_return(0xA0u, return_pc, 0u, at_target.gpr[R_V1]);
  oracle_capture(&after_refusal);
  check_u32("modeled return: pending load is refused", (uint32_t)pending_load, 0u);
  check_u32("modeled return: load-delay refusal preserves PC", after_refusal.pc, at_target.pc);
  oracle_teardown();
  return 1;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0); // unbuffered: a crash mid-run must not swallow the checks already
  setvbuf(stderr, NULL, _IONBF, 0); // printed, and stderr/stdout must interleave in the real order

  printf("psxport oracle spike — milestone 1 of docs/plans/oracle-against-beetle.md\n");
  printf("PLAN: %d checks across 10 program classes.\n", PLANNED_CHECKS);
  printf("  POSITIVE (9 checks): inject 8 hand-assembled instructions at 0x%08X, run 200 cycles,\n"
         "    assert $t0-$t3, $gp, $sp, the stored word in RAM, a clean stop, and that PC advanced within RAM.\n",
         FIX_ADDR);
  printf("  NEGATIVE (3 checks): read GPUSTAT at 0x1F801814 and assert the run REPORTS a hardware\n"
         "    stop at that address, rather than reading 0 and continuing.\n");
  printf("  IRQ BUS (4 checks): write/read I_MASK and write I_STAT through the vendored controller,\n"
         "    then prove the same CPU continues; GPUSTAT remains the opposite answer.\n");
  printf("  DMA BUS (13 checks): require the hardware reset and partial-lane semantics, write/read DPCR\n"
         "    through the vendored controller, and require adjacent DICR to stop and taint later capture.\n");
  printf("  DEVICE BOUNDARY (9 checks): run the generic I_MASK/I_STAT/DPCR sequence through a real jal,\n"
         "    then capture distinct valid/write provenance and all three device values before the callee.\n");
  printf("  EVENT TAINT (2 checks): schedule an unsupported event and require both device capture and a\n"
         "    later step to retain the event refusal instead of laundering the invalid window.\n");
  printf("  STEPPING (6 checks): run the SAME fixture one instruction at a time and require it to land\n"
         "    exactly where the bulk run landed, having actually stepped through the program.\n");
  printf("  MIRRORING (4 checks): store through 0x807FFFF8 — the address Spider-Man's crt0 really uses as\n"
         "    its stack pointer — and require it to be RAM via the 4th mirror, not a hardware access.\n");
  printf("  MODELED RETURN (17 checks): reach an external leaf through a real call/thunk, require wrong\n"
         "    target/return and pending-load boundaries to refuse, then explicitly return and execute the caller.\n");
  printf("  SYSCALL RETURN (17 checks): let the CPU produce CP0 exception state, refuse wrong selector\n"
         "    and delay-slot state, then pop SR once, install results, and resume after EPC.\n");
  printf("  BLIND SPOTS, stated so this is not mistaken for more than it is: no BIOS is mapped, no\n"
         "    real game executable is loaded, no comparison against psxport's own paths is performed,\n"
         "    and nothing here exercises the GTE, DMA channels/DICR, CD or timers.\n\n");

  printf("POSITIVE — a program whose results are derived by hand in this file:\n");
  if (!positive_case()) {
    printf("  REFUSED: oracle setup failed; the positive case did not run.\n");
    return 2;
  }

  printf("\nNEGATIVE — a hardware access must end the window and be named:\n");
  if (!negative_case()) {
    printf("  REFUSED: oracle setup failed; the negative case did not run.\n");
    return 2;
  }

  printf("\nIRQ BUS — the vendored controller must execute write/read/write without leaving this CPU:\n");
  if (!irq_bus_case()) {
    printf("  REFUSED: oracle setup failed; the IRQ-bus case did not run.\n");
    return 2;
  }

  printf("\nDMA BUS — the vendored controller must execute DPCR write/read without leaving this CPU:\n");
  if (!dma_bus_case()) {
    printf("  REFUSED: oracle setup failed; the DMA-bus case did not run.\n");
    return 2;
  }

  printf("\nDEVICE BOUNDARY — CPU and device state remain independent evidence surfaces:\n");
  if (!device_boundary_case()) {
    printf("  REFUSED: oracle setup failed; the device-boundary case did not run.\n");
    return 2;
  }

  printf("\nEVENT TAINT — a dropped scheduled event stays a refusal until reload:\n");
  if (!event_taint_case()) {
    printf("  REFUSED: oracle setup failed; the event-taint case did not run.\n");
    return 2;
  }

  printf("\nSTEPPING — one instruction at a time must equal one bulk run:\n");
  if (!stepping_case()) {
    printf("  REFUSED: oracle setup failed; the stepping case did not run.\n");
    return 2;
  }

  printf("\nMIRRORING — 2 MB of RAM appears four times across the 8 MB window:\n");
  if (!mirroring_case()) {
    printf("  REFUSED: oracle setup failed; the mirroring case did not run.\n");
    return 2;
  }

  printf("\nMODELED RETURN — explicit external-leaf semantics resume this same CPU:\n");
  if (!modeled_call_return_case()) {
    printf("  REFUSED: oracle setup failed; the modeled-return case did not run.\n");
    return 2;
  }

  printf("\nSYSCALL RETURN — CP0 validates an exception before the caller supplies semantics:\n");
  if (!syscall_return_case()) {
    printf("  REFUSED: oracle setup failed; the syscall-return case did not run.\n");
    return 2;
  }

  printf("\n%d checks planned, %d ran, %d failed.\n", PLANNED_CHECKS, s_ran, s_failed);
  if (s_ran != PLANNED_CHECKS) {
    printf("REFUSING to report a result: %d checks ran but %d were planned, so this run does not cover\n"
           "what it claims to. Fix the plan or the code path that skipped a check.\n",
           s_ran,
           PLANNED_CHECKS);
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

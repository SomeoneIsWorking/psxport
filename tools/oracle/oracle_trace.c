// oracle_trace.c — run a REAL game executable in the independent reference emulator and write a
// per-instruction trace. Milestone 2 of `docs/plans/oracle-against-beetle.md`.
//
// ═══ WHY A TRACE FILE, AND NOT ONE PROCESS RUNNING BOTH SIDES ════════════════════════════════════════
// The plan recorded two ways to keep the reference independent of the thing being tested — a separate
// process with a pipe, or `objcopy --prefix-symbols` on the archive — because `libpsxport` already
// compiles `gte.c` for its own GTE backend, so linking both into one executable collides. A trace FILE is
// better than either:
//   * it is a separate process, so the reference and the port cannot share one byte of state — which is
//     the entire point of having an independent reference;
//   * it needs no IPC protocol, no lockstep handshake, and no ordering assumptions between two running
//     emulators;
//   * it is REPRODUCIBLE and INSPECTABLE. A pipe's contents exist only while both ends live; a trace can
//     be diffed, re-diffed after a recompiler change, read by a human, and checked into an issue.
//   * a compare that fails can be re-run against the SAME reference bytes, so "did the oracle change or
//     did we?" is answerable. With two live processes it is not.
// The port still cannot steer this process. A CONSUMER can explicitly model one external BIOS return,
// however: the trace validates the table/function boundary, applies the generic shim's checked call-return
// continuation, and captures the next observable boundary. Policy remains outside this process and the two
// executions still share no state.
//
// ═══ WHAT IT DOES NOT DO ═════════════════════════════════════════════════════════════════════════════
// It does not compare anything. It produces one side of a comparison. A trace being written successfully
// says the reference ran; it says nothing about whether our port agrees with it.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oracle_shim.h"

// PS-X EXE header fields, read as little-endian words at fixed offsets. This is deliberately NOT a second
// crt0 decoder: `tools/crt0_extract` remains the only thing that INTERPRETS the boot prologue (bss span,
// heap base, stack bias), and this tool never touches those. All it needs is where the image loads and the
// three registers the loader sets, which are stored verbatim in the header — so there is no derivation
// here that could drift from the decoder.
#define OFF_PC0 0x10
#define OFF_GP0 0x14
#define OFF_T_ADDR 0x18
#define OFF_T_SIZE 0x1C
#define OFF_SP 0x30
#define PSX_EXE_HEADER_BYTES 0x800

static uint32_t rd32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Register names, so a trace reads as MIPS rather than as `r29`. A trace nobody can read gets diffed by
// machine and understood by nobody, and every divergence then costs a manual lookup.
static const char *kReg[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                               "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                               "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

typedef struct ModeledBiosReturn {
  int enabled;
  char table;
  uint32_t target;
  uint32_t function;
  uint32_t v0;
} ModeledBiosReturn;

static int parse_u32(const char *text, const char **end, uint32_t *value) {
  char *parsed_end = NULL;
  const unsigned long parsed = strtoul(text, &parsed_end, 0);
  if (parsed_end == text || parsed > UINT32_MAX) {
    return 0;
  }
  *end = parsed_end;
  *value = (uint32_t)parsed;
  return 1;
}

static int parse_modeled_bios_return(const char *text, ModeledBiosReturn *model) {
  if (!text || !text[0] || text[1] != ':') {
    return 0;
  }
  char table = text[0];
  if (table >= 'a' && table <= 'z') {
    table = (char)(table - ('a' - 'A'));
  }
  if (table != 'A' && table != 'B' && table != 'C') {
    return 0;
  }

  const char *cursor = text + 2;
  const char *end = NULL;
  uint32_t function = 0, v0 = 0;
  if (!parse_u32(cursor, &end, &function) || *end != ':' || function > 0xFFu) {
    return 0;
  }
  cursor = end + 1;
  if (!parse_u32(cursor, &end, &v0) || *end != '\0') {
    return 0;
  }

  model->enabled = 1;
  model->table = table;
  model->target = table == 'A' ? 0xA0u : table == 'B' ? 0xB0u : 0xC0u;
  model->function = function;
  model->v0 = v0;
  return 1;
}

static void
dump_register_block(FILE *out, const char *tag, const char *position_name, long position, const OracleState *state) {
  fprintf(out, "# %s-REGS %s=%ld pc=0x%08X\n", tag, position_name, position, state->pc);
  for (int r = 1; r < 32; r++) {
    fprintf(out, "# %s-REG %s=0x%08X\n", tag, kReg[r], state->gpr[r]);
  }
  fprintf(out, "# %s-REG lo=0x%08X\n# %s-REG hi=0x%08X\n", tag, state->lo, tag, state->hi);
}

static void dump_pc_boundary(FILE *out, uint32_t target, long executed, const OracleState *state) {
  fprintf(out, "# CAPTURED-PC target=0x%08X executed=%ld\n", target, executed);
  dump_register_block(out, "PC-BOUNDARY", "executed", executed, state);
}

static void usage(void) {
  fprintf(stderr,
          "usage: oracle_trace <PS-X EXE> [--steps N] [--out FILE] [--entry 0xADDR]\n"
          "                    [--capture-first-call | --capture-call N | --capture-at 0xADDR]\n"
          "                    [--model-bios-return TABLE:FN:V0]\n"
          "\n"
          "Steps a real game executable in the independent reference emulator (the vendored Mednafen PSX\n"
          "CPU, no libretro.c) and writes a per-instruction trace for a differential compare.\n"
          "\n"
          "  --steps N      how many instructions to trace (default 200)\n"
          "  --out FILE     where the trace goes (default: stdout)\n"
          "  --entry 0xADDR start somewhere other than the header's pc0, for tracing one function\n"
          "  --capture-first-call record registers after the delay slot of the first executed jal;\n"
          "                       compatibility alias for --capture-call 1\n"
          "  --capture-call N record the Nth executed jal after its delay slot; refuse with the\n"
          "                   reached denominator if fewer than N calls occur inside --steps\n"
          "  --capture-at ADDR stop when the CPU reaches ADDR and record state before executing\n"
          "                    its instruction; refuse with the executed-step denominator if it is\n"
          "                    not reached. This is mutually exclusive with call/model capture.\n"
          "  --model-bios-return TABLE:FN:V0\n"
          "                 when execution reaches the named A0/B0/C0 vector with $t1 == FN, install\n"
          "                 explicit $v0 (preserving $v1), resume at the observed $ra, and capture the\n"
          "                 first subsequent call or hardware access. The consumer owns the BIOS\n"
          "                 semantics; this tool only validates and applies the modeled return.\n"
          "  --summary-only omit per-step lines; keep the headers, the boundary register dump and the\n"
          "                 summary. For long windows (a bss-zeroing loop is ~200k instructions) where the\n"
          "                 per-step detail would be gigabytes and only the boundary is of interest.\n"
          "\n"
          "The trace has one line per step: `<n> <pc> <instr> [reg=value ...]`, listing only the registers\n"
          "that CHANGED. A step that changes nothing still gets a line, because a missing line and an\n"
          "unchanged step must not look the same to a diff.\n"
          "\n"
          "It writes NO trace and exits 2 if the image cannot be loaded — a truncated trace that looks\n"
          "complete is worse than none. docs/plans/oracle-against-beetle.md\n");
}

int main(int argc, char **argv) {
  const char *path = NULL, *out_path = NULL;
  long steps = 200;
  uint32_t entry_override = 0;
  long capture_call_ordinal = 0;
  uint32_t capture_pc = 0;
  int capture_pc_enabled = 0;
  int summary_only = 0;
  ModeledBiosReturn modeled_bios = {0};

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--steps") && i + 1 < argc) {
      steps = strtol(argv[++i], NULL, 0);
    } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
      out_path = argv[++i];
    } else if (!strcmp(argv[i], "--entry") && i + 1 < argc) {
      entry_override = (uint32_t)strtoul(argv[++i], NULL, 0);
    } else if (!strcmp(argv[i], "--capture-first-call")) {
      if (capture_pc_enabled || capture_call_ordinal > 1) {
        fprintf(stderr, "oracle_trace: --capture-first-call conflicts with another capture selector.\n");
        return 2;
      }
      capture_call_ordinal = 1;
    } else if (!strcmp(argv[i], "--capture-call") && i + 1 < argc) {
      char *end = NULL;
      const long ordinal = strtol(argv[++i], &end, 0);
      if (!end || *end != '\0' || ordinal <= 0 || capture_pc_enabled ||
          (capture_call_ordinal && capture_call_ordinal != ordinal)) {
        fprintf(stderr, "oracle_trace: invalid or conflicting --capture-call ordinal %s.\n", argv[i]);
        return 2;
      }
      capture_call_ordinal = ordinal;
    } else if (!strcmp(argv[i], "--capture-at") && i + 1 < argc) {
      const char *end = NULL;
      uint32_t parsed_pc = 0;
      if (!parse_u32(argv[++i], &end, &parsed_pc) || *end != '\0' || (parsed_pc & 3u) != 0 || capture_call_ordinal ||
          modeled_bios.enabled || (capture_pc_enabled && capture_pc != parsed_pc)) {
        fprintf(stderr, "oracle_trace: invalid, unaligned, or conflicting --capture-at address %s.\n", argv[i]);
        return 2;
      }
      capture_pc = parsed_pc;
      capture_pc_enabled = 1;
    } else if (!strcmp(argv[i], "--model-bios-return") && i + 1 < argc) {
      if (capture_pc_enabled || !parse_modeled_bios_return(argv[++i], &modeled_bios)) {
        fprintf(stderr, "oracle_trace: invalid --model-bios-return %s (expected A:0x39:0 style).\n", argv[i]);
        return 2;
      }
    } else if (!strcmp(argv[i], "--summary-only")) {
      summary_only = 1;
    } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      usage();
      return 0;
    } else if (argv[i][0] == '-') {
      fprintf(stderr, "oracle_trace: unknown option %s\n", argv[i]);
      usage();
      return 2;
    } else if (!path) {
      path = argv[i];
    } else {
      fprintf(stderr, "oracle_trace: unexpected extra argument %s\n", argv[i]);
      usage();
      return 2;
    }
  }
  if (!path) {
    usage();
    return 2;
  }
  if (steps <= 0) {
    fprintf(stderr,
            "oracle_trace: REFUSING — --steps %ld traces nothing. An empty trace would read as a\n"
            "  run that agreed at every instruction it compared, which is zero instructions.\n",
            steps);
    return 2;
  }

  // ── load the image, refusing rather than tracing garbage ────────────────────────────────────────
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "oracle_trace: REFUSING — cannot open %s. No trace written.\n", path);
    return 2;
  }
  fseek(f, 0, SEEK_END);
  long fsz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (fsz < PSX_EXE_HEADER_BYTES) {
    fprintf(stderr,
            "oracle_trace: REFUSING — %s is %ld byte(s), shorter than the 0x800 PS-X EXE header.\n"
            "  No trace written.\n",
            path,
            fsz);
    fclose(f);
    return 2;
  }
  uint8_t *img = (uint8_t *)malloc((size_t)fsz);
  if (!img || fread(img, 1, (size_t)fsz, f) != (size_t)fsz) {
    fprintf(stderr, "oracle_trace: REFUSING — could not read all %ld byte(s) of %s. No trace written.\n", fsz, path);
    fclose(f);
    free(img);
    return 2;
  }
  fclose(f);

  if (memcmp(img, "PS-X EXE", 8) != 0) {
    fprintf(stderr,
            "oracle_trace: REFUSING — %s does not begin with the PS-X EXE magic (got %.8s).\n"
            "  A packed or headerless file would be injected at the wrong address and every\n"
            "  instruction traced would be at a wrong PC. No trace written.\n",
            path,
            (const char *)img);
    free(img);
    return 2;
  }

  const uint32_t pc0 = rd32le(img + OFF_PC0);
  const uint32_t gp0 = rd32le(img + OFF_GP0);
  const uint32_t t_addr = rd32le(img + OFF_T_ADDR);
  const uint32_t t_size = rd32le(img + OFF_T_SIZE);
  const uint32_t sp0 = rd32le(img + OFF_SP);
  const uint32_t entry = entry_override ? entry_override : pc0;

  const uint32_t avail = (uint32_t)(fsz - PSX_EXE_HEADER_BYTES);
  const uint32_t present = t_size <= avail ? t_size : avail;

  if (entry < t_addr || entry >= t_addr + present) {
    fprintf(stderr,
            "oracle_trace: REFUSING — the entry 0x%08X is OUTSIDE the mapped image "
            "0x%08X..0x%08X.\n  Every fetch would read zeroed RAM and the trace would record a run "
            "of nops as if it had\n  traced the game. No trace written.\n",
            entry,
            t_addr,
            t_addr + present);
    free(img);
    return 2;
  }

  // ── bring up the reference and inject ───────────────────────────────────────────────────────────
  if (!oracle_init()) {
    free(img);
    return 2;
  }
  // The header's sp is used verbatim. Note it is the RAW header value: our port's `crt0_apply` may apply a
  // measured stack BIAS on top (Spyro's is -8, per crt0_extract), and the real crt0 applies that bias with
  // its own instructions — which the oracle is about to EXECUTE. Pre-biasing here would apply it twice.
  if (!oracle_load_exe(img + PSX_EXE_HEADER_BYTES, present, t_addr, entry, gp0, sp0)) {
    free(img);
    oracle_teardown();
    return 2;
  }

  FILE *out = stdout;
  if (out_path) {
    out = fopen(out_path, "w");
    if (!out) {
      fprintf(stderr, "oracle_trace: REFUSING — cannot write %s. No trace written.\n", out_path);
      free(img);
      oracle_teardown();
      return 2;
    }
  }

  fprintf(out, "# oracle_trace of %s\n", path);
  fprintf(out, "# reference: vendored Mednafen PSX CPU, no libretro.c, no BIOS mapped\n");
  fprintf(out,
          "# header: pc0=0x%08X gp0=0x%08X sp=0x%08X text=0x%08X..0x%08X (0x%X of 0x%X byte(s) present)\n",
          pc0,
          gp0,
          sp0,
          t_addr,
          t_addr + present,
          present,
          t_size);
  fprintf(out, "# entry: 0x%08X%s\n", entry, entry_override ? " (--entry override)" : " (header pc0)");
  fprintf(out, "# steps requested: %ld\n", steps);
  if (capture_call_ordinal) {
    fprintf(out, "# requested call capture: executed jal ordinal %ld\n", capture_call_ordinal);
  }
  if (capture_pc_enabled) {
    fprintf(out, "# requested PC capture: 0x%08X before its instruction executes\n", capture_pc);
  }
  if (modeled_bios.enabled) {
    fprintf(out,
            "# requested modeled BIOS return: %c(0x%02X) at 0x%08X, explicit v0=0x%08X, v1 preserved\n",
            modeled_bios.table,
            modeled_bios.function,
            modeled_bios.target,
            modeled_bios.v0);
  }
  fprintf(out, "# format: <n> <pc> <cycles> [reg=value ...]  — only CHANGED registers are listed\n");

  OracleState prev;
  oracle_capture(&prev);
  fprintf(out, "# initial: pc=0x%08X gp=0x%08X sp=0x%08X\n", prev.pc, prev.gpr[28], prev.gpr[29]);

  long n = 0;
  long executed_steps = 0;
  uint32_t left_text_at = 0; // the first PC outside the mapped text, if any
  long left_text_step = -1;
  OracleStop stop = ORACLE_STOP_NONE;

  // The most recent `jal`, tracked so the boundary record can name the CALL that left the text. A jal
  // writes $ra and then the DELAY SLOT executes, so the target is the PC one step LATER — which is why
  // this cannot be read off the boundary register file. `$ra - 4` is the jal site in the CALLER, not the
  // target; that mistake produced a false DISAGREE in the cross-check before this record existed.
  uint32_t last_jal_target = 0, last_jal_ra = 0;
  long last_jal_step = -1;
  int jal_pending = 0;
  uint32_t jal_pending_ra = 0;
  long captured_call_step = -1;
  long captured_pc_executed = -1;
  long completed_jal_calls = 0;
  long modeled_return_step = -1;
  long post_return_call_step = -1;
  long post_return_hardware_step = -1;
  int modeled_return_refused = 0;
  int post_call_pending = 0;
  uint32_t post_call_pending_ra = 0;

  if (capture_pc_enabled && prev.pc == capture_pc) {
    captured_pc_executed = 0;
    dump_pc_boundary(out, capture_pc, captured_pc_executed, &prev);
  }

  for (n = 0; n < steps && captured_pc_executed < 0; n++) {
    // Identify jal from the instruction bytes, not from a change to $ra: ordinary instructions and jalr
    // can also write r31. The executable bytes are independent of the symbolic crt0 decoder.
    int executed_jal = 0;
    int executed_link_call = 0;
    if (present >= 4 && prev.pc >= t_addr && prev.pc <= t_addr + present - 4) {
      const uint32_t offset = prev.pc - t_addr;
      const uint32_t instruction = rd32le(img + PSX_EXE_HEADER_BYTES + offset);
      const uint32_t opcode = instruction >> 26;
      executed_jal = opcode == 3;
      executed_link_call =
          executed_jal || (opcode == 0 && (instruction & 0x3Fu) == 9u && ((instruction >> 11) & 0x1Fu) != 0);
    }
    stop = oracle_step();
    OracleState now;
    oracle_capture(&now);
    executed_steps++;

    // Every step gets a line, even one that changed nothing: a diff must be able to tell "this step made
    // no difference" apart from "this step is missing from the trace".
    if (!summary_only) {
      fprintf(out, "%ld 0x%08X %d", n, now.pc, now.timestamp);
      for (int r = 1; r < 32; r++) { // r0 is hardwired 0; a change there would be a core bug
        if (now.gpr[r] != prev.gpr[r]) {
          fprintf(out, " %s=0x%08X", kReg[r], now.gpr[r]);
        }
      }
      if (now.lo != prev.lo) {
        fprintf(out, " lo=0x%08X", now.lo);
      }
      if (now.hi != prev.hi) {
        fprintf(out, " hi=0x%08X", now.hi);
      }
      fputc('\n', out);
    }

    // Leaving the mapped text is the interesting event, not an error: it is where a straight-line window
    // ENDS, and the plan's whole option-1 design turns on knowing exactly where that is. Recorded on FIRST
    // occurrence, because a later re-entry must not overwrite where the window actually broke.
    if (left_text_step < 0 && (now.pc < t_addr || now.pc >= t_addr + present)) {
      left_text_at = now.pc;
      left_text_step = n;
      fprintf(out,
              "# LEFT THE MAPPED TEXT at step %ld: pc=0x%08X is outside 0x%08X..0x%08X\n",
              n,
              now.pc,
              t_addr,
              t_addr + present);
      // THE FULL REGISTER FILE at the boundary, in one block. This is the artifact a cross-check wants:
      // the boot group our own `crt0_plan` DERIVES symbolically (gp, sp, the libcInit target, and the
      // a0/a1 handed to InitHeap) is visible here as the values the real code actually produced. Dumped
      // unconditionally, including under --summary-only, because the boundary is the whole point of a
      // long window — and dumped in one place so a cross-check parses one format, not a scan for the
      // last write to each register.
      dump_register_block(out, "BOUNDARY", "step", n, &now);
      if (last_jal_step >= 0) {
        fprintf(
            out, "# BOUNDARY-LAST-JAL target=0x%08X ra=0x%08X step=%ld\n", last_jal_target, last_jal_ra, last_jal_step);
      } else {
        fprintf(out,
                "# BOUNDARY-LAST-JAL none — execution left the text without a jal having been\n"
                "#   observed, so there is no call site to attribute the boundary to\n");
      }
    }
    if (jal_pending) { // the delay slot has now run; this PC is the call target
      last_jal_target = now.pc;
      last_jal_ra = jal_pending_ra;
      last_jal_step = n;
      jal_pending = 0;
      completed_jal_calls++;
      if (capture_call_ordinal == completed_jal_calls && captured_call_step < 0) {
        captured_call_step = n;
        fprintf(out, "# CAPTURED-CALL target=0x%08X ra=0x%08X step=%ld\n", last_jal_target, last_jal_ra, last_jal_step);
        dump_register_block(out, "CALL-BOUNDARY", "step", n, &now);
      }
    }
    if (post_call_pending) {
      post_return_call_step = n;
      post_call_pending = 0;
      fprintf(out, "# POST-RETURN-CAPTURED-CALL target=0x%08X ra=0x%08X step=%ld\n", now.pc, post_call_pending_ra, n);
      dump_register_block(out, "POST-RETURN-CALL-BOUNDARY", "step", n, &now);
    }

    if (modeled_bios.enabled && modeled_return_step < 0 && now.pc == modeled_bios.target) {
      if ((now.gpr[9] & 0xFFu) != modeled_bios.function) {
        fprintf(stderr,
                "oracle_trace: REFUSING modeled BIOS return — reached %c0 at step %ld with $t1=0x%08X, "
                "not requested function 0x%02X.\n",
                modeled_bios.table,
                n,
                now.gpr[9],
                modeled_bios.function);
        modeled_return_refused = 1;
      } else if (!oracle_resume_call_return(modeled_bios.target, now.gpr[31], modeled_bios.v0, now.gpr[3])) {
        modeled_return_refused = 1;
      } else {
        const uint32_t return_pc = now.gpr[31];
        const uint32_t preserved_v1 = now.gpr[3];
        modeled_return_step = n;
        post_call_pending = 0;
        oracle_capture(&now);
        fprintf(out,
                "# MODELED-BIOS-RETURN table=%c function=0x%02X target=0x%08X ra=0x%08X "
                "v0=0x%08X v1=0x%08X step=%ld\n",
                modeled_bios.table,
                modeled_bios.function,
                modeled_bios.target,
                return_pc,
                modeled_bios.v0,
                preserved_v1,
                n);
        dump_register_block(out, "MODELED-RETURN", "step", n, &now);
      }
    }
    if (executed_jal) {
      jal_pending = 1;
      jal_pending_ra = now.gpr[31];
    }
    if (executed_link_call && modeled_return_step >= 0 && post_return_call_step < 0) {
      post_call_pending = 1;
      post_call_pending_ra = now.gpr[31];
    }

    if (stop == ORACLE_STOP_HARDWARE && modeled_return_step >= 0 && post_return_call_step < 0) {
      post_return_hardware_step = n;
      fprintf(out, "# POST-RETURN-HARDWARE address=0x%08X step=%ld\n", now.stop_addr, n);
      dump_register_block(out, "POST-RETURN-HARDWARE-BOUNDARY", "step", n, &now);
    }

    // A device callback can advance BACKED_PC before ending the slice. That successor is not a valid
    // capture boundary: the unsupported predecessor did not produce trustworthy state. Only a cleanly
    // completed instruction may establish the requested pre-execution PC boundary.
    if (capture_pc_enabled && stop == ORACLE_STOP_BUDGET && now.pc == capture_pc) {
      captured_pc_executed = executed_steps;
      dump_pc_boundary(out, capture_pc, captured_pc_executed, &now);
      break;
    }

    if (modeled_return_refused || post_return_call_step >= 0 || post_return_hardware_step >= 0 ||
        stop != ORACLE_STOP_BUDGET) {
      break;
    }
    prev = now;
  }

  OracleState fin;
  oracle_capture(&fin);
  const long traced = executed_steps;

  // ── the summary states the DENOMINATOR and every reason the trace is shorter than asked for ─────
  fprintf(out,
          "# traced %ld of %ld requested step(s), %d cycle(s), ended pc=0x%08X\n",
          traced,
          steps,
          fin.timestamp,
          fin.pc);
  fprintf(out, "# stop reason: %s\n", oracle_stop_name(stop));
  if (stop == ORACLE_STOP_HARDWARE) {
    fprintf(out, "# hardware address: 0x%08X\n", fin.stop_addr);
  }
  if (left_text_step >= 0) {
    if (modeled_return_step >= 0) {
      fprintf(out,
              "# left mapped text at step %ld (pc=0x%08X), then resumed at the explicit modeled-call\n"
              "#   return boundary at the same step; no external-vector instructions were executed\n",
              left_text_step,
              left_text_at);
    } else {
      fprintf(out,
              "# left mapped text at step %ld (pc=0x%08X); everything after that is NOT game code from\n"
              "#   this image, and no compare should treat it as such\n",
              left_text_step,
              left_text_at);
    }
  } else {
    fprintf(out,
            "# NEVER LEFT the mapped text in %ld traced step(s) — no boundary was reached, so this\n"
            "#   trace contains NO BOUNDARY-REG block and a cross-check against the boot group cannot\n"
            "#   be performed from it. Raise --steps.\n",
            traced);
  }
  if (capture_call_ordinal) {
    if (captured_call_step >= 0) {
      fprintf(out,
              "# captured executed jal call %ld of %ld requested at step %ld; observed %ld total\n",
              capture_call_ordinal,
              capture_call_ordinal,
              captured_call_step,
              completed_jal_calls);
    } else {
      fprintf(out,
              "# REQUESTED CALL WAS NOT REACHED in %ld traced step(s): reached %ld of %ld executed jal\n"
              "#   call boundary/boundaries; no CALL-BOUNDARY block was written. Raise --steps.\n",
              traced,
              completed_jal_calls,
              capture_call_ordinal);
    }
  }
  if (capture_pc_enabled) {
    if (captured_pc_executed >= 0) {
      fprintf(out,
              "# requested PC 0x%08X reached after %ld executed instruction(s); its instruction was not executed\n",
              capture_pc,
              captured_pc_executed);
    } else {
      fprintf(out,
              "# REQUESTED PC WAS NOT REACHED in %ld of %ld requested executed instruction(s): "
              "pc=0x%08X; no PC-BOUNDARY block was written. Raise --steps.\n",
              traced,
              steps,
              capture_pc);
    }
  }
  if (modeled_bios.enabled) {
    if (modeled_return_step < 0) {
      fprintf(out,
              "# MODELED BIOS RETURN WAS NOT APPLIED in %ld traced step(s); no post-return comparison\n"
              "#   can be performed\n",
              traced);
    } else if (post_return_call_step >= 0) {
      fprintf(out,
              "# post-return boundary: call captured at step %ld after modeled return at step %ld\n",
              post_return_call_step,
              modeled_return_step);
    } else if (post_return_hardware_step >= 0) {
      fprintf(out,
              "# post-return boundary: hardware captured at step %ld after modeled return at step %ld\n",
              post_return_hardware_step,
              modeled_return_step);
    } else {
      fprintf(out,
              "# NO POST-RETURN CALL OR HARDWARE was reached after modeled return at step %ld; raise --steps\n",
              modeled_return_step);
    }
  }
  if (summary_only) {
    fprintf(out,
            "# --summary-only: per-step lines were omitted deliberately. This trace CANNOT be used\n"
            "#   for a per-instruction differential; it carries the boundary and the summary only.\n");
  }

  if (out != stdout) {
    fclose(out);
  }

  // The human-facing summary goes to stderr so it never contaminates a trace written to stdout.
  fprintf(stderr,
          "oracle_trace: %ld of %ld step(s) traced, %d cycle(s), ended pc=0x%08X (%s)\n",
          traced,
          steps,
          fin.timestamp,
          fin.pc,
          oracle_stop_name(stop));
  if (left_text_step >= 0) {
    if (modeled_return_step >= 0) {
      fprintf(stderr,
              "  reached external pc=0x%08X at step %ld, applied the explicit modeled return, and\n"
              "  resumed the same independent CPU without executing vector memory.\n",
              left_text_at,
              left_text_step);
    } else {
      fprintf(stderr,
              "  left the mapped text at step %ld -> pc=0x%08X. That is the end of the straight-line\n"
              "  window; see the plan's BIOS-call boundary (milestone 3).\n",
              left_text_step,
              left_text_at);
    }
  }
  if (out_path) {
    fprintf(stderr, "  trace: %s\n", out_path);
  }

  if (capture_call_ordinal && captured_call_step < 0) {
    fprintf(stderr,
            "oracle_trace: REFUSING — reached %ld of %ld requested executed jal call boundary/boundaries "
            "in %ld step(s).\n",
            completed_jal_calls,
            capture_call_ordinal,
            traced);
  }
  if (capture_pc_enabled && captured_pc_executed < 0) {
    fprintf(stderr,
            "oracle_trace: REFUSING — requested PC 0x%08X was not reached in %ld of %ld requested "
            "executed instruction(s).\n",
            capture_pc,
            traced,
            steps);
  }

  const int modeled_incomplete =
      modeled_bios.enabled && (modeled_return_step < 0 || (post_return_call_step < 0 && post_return_hardware_step < 0));
  if (modeled_incomplete || modeled_return_refused) {
    fprintf(stderr,
            "oracle_trace: REFUSING — the requested modeled return and post-return boundary were not both proven.\n");
  }

  free(img);
  oracle_teardown();
  return (capture_call_ordinal && captured_call_step < 0) || (capture_pc_enabled && captured_pc_executed < 0) ||
                 modeled_incomplete || modeled_return_refused
             ? 2
             : 0;
}

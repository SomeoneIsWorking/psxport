// oracle_shim.c — the minimum host the vendored Mednafen PSX CPU needs in order to STEP, and no more.
//
// WHY THIS FILE EXISTS. A reference implemented by the product can share the product's mistakes.
// This tool instead hosts the already-vendored independent Mednafen PSX CPU. Its system glue lives
// in `vendor/beetle-psx/libretro.c`, which also owns the disc,
// the BIOS, the video frontend and the libretro option system. This file replaces that glue with the
// smallest surface that lets the CPU execute instructions out of a RAM image we control.
//
// MEASURED GLUE SURFACE (2026-08-13, by linking rather than by reading): stepping needs `cpu.o`,
// `gte.o` and the six PGXP objects, and those reference exactly FIFTEEN symbols nobody in that set
// defines — `ScratchRAM`, the eight `PSX_MemRead/Write*`, `PSX_EventHandler`, `psx_gte_overclock`,
// `MDFNSS_StateAction`, `widescreen_hack`, `widescreen_hack_aspect_ratio_setting`. The narrow bus
// extensions also link Mednafen's `irq.o` and the factored DPCR register owner; DMA channels, DICR,
// CD, GPU, timer, SIO and filestream layers remain outside this stepping library.
// `MainRAM` is here because THIS file owns the RAM, not because the CPU asks for it — the CPU reaches
// main memory through `PSX_MemRead*` and through the FastMap set up in `oracle_init`.
//
// ═══ THE RULE HERE: A STUB THAT IS REACHED MUST SAY SO, NEVER RETURN ZERO ═════════════════════════════
// Milestone 1 began with straight-line code; the oracle now models only the PSX interrupt controller's
// I_STAT/I_MASK range and DPCR's four byte lanes. Those owners let real register sequences stay on the
// same Mednafen CPU without pulling in unrelated devices. Every other hardware access remains a
// measurement boundary: `PSX_MemRead*`/`PSX_MemWrite*` name the address and report
// `ORACLE_STOP_HARDWARE` rather than inventing a value. Returning 0 there would make "the window ended"
// indistinguishable from "a device held zero".
//
// ═══ DETERMINISM ═════════════════════════════════════════════════════════════════════════════════════
// The plan makes determinism a precondition, so: nothing here reads host time or host entropy, and
// nothing may schedule work inside the window (see `PSX_SetEventNT`). The window's end is set by ONE
// synthetic clock — the core's own `cpu_next_event_ts`, which `oracle_run` writes — which is the plan's
// "run a window that touches no counters" option enforced rather than hoped for.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "dma_dpcr.h"
#include "irq.h"
#include "oracle_shim.h"
#include "psx.h"

// `cpu_next_event_ts` is a non-static global in cpu.c (line 112). CPU_Run's inner loop is
// `while (timestamp < next_event_ts)`, so writing it is how a host declares where a timeslice ends —
// the same lever libretro.c pulls, reached directly instead of through the event scheduler.
extern int32_t cpu_next_event_ts;

// ── the memory the reference owns ──────────────────────────────────────────────────────────────────
MultiAccessSizeMem *MainRAM = NULL;    // 2 MB, mirrored across KUSEG/KSEG0/KSEG1
MultiAccessSizeMem *BIOSROM = NULL;    // deliberately NULL: milestone 1 executes NO BIOS on either side
MultiAccessSizeMem *ScratchRAM = NULL; // 1 KB at 0x1F800000

// Settings globals libretro.c owns. Values are the neutral ones, written explicitly rather than left to
// zero-initialisation, because "off because nobody set it" and "off on purpose" read identically in a
// debugger and only one of them is a decision.
int32_t psx_gte_overclock = 0; // stock GTE timing; the reference must not be faster
uint8_t widescreen_hack = 0;   // no geometry change in a byte-compare reference
uint8_t widescreen_hack_aspect_ratio_setting = 0;

#define RAM_SIZE 0x00200000u // 2 MB of physical main RAM
// Main RAM is MIRRORED FOUR TIMES across the 8 MB window, and that is not a detail: the reference's own
// decoder is `if (A < 0x00800000) ... MASMEM_*(MainRAM, A & 0x1FFFFF)` (libretro.c:1085-1108), i.e. the
// region test is 8 MB wide and the offset wraps at 2 MB.
//
// MEASURED CONSEQUENCE (2026-08-13): Spider-Man's crt0 computes `sp = 0x807FFFF8` — its stack-top global
// holds 0x00800000 — which is the top of RAM through the FOURTH mirror and entirely legitimate. This shim
// originally tested `phys < RAM_SIZE`, so any stack access Spider-Man made would have been reported as a
// HARDWARE ACCESS and ended the window at a boundary that does not exist. Worse, it was INCONSISTENT with
// this file's own FastMap setup, which mirrors 4x (copied from libretro.c) — so instruction fetch was
// right while the data path was wrong, and only a game with a high stack would ever have shown it.
#define RAM_WINDOW 0x00800000u // the mirrored window the reference decodes as main RAM
#define RAM_MASK 0x001FFFFFu   // and the offset it wraps to
#define SPAD_BASE 0x1F800000u
#define SPAD_SIZE 0x00000400u
#define IRQ_BASE 0x1F801070u
#define IRQ_END 0x1F801078u
#define DMA_DPCR_BASE 0x1F8010F0u
#define DMA_DPCR_END 0x1F8010F4u
#define SYSCALL_VECTOR 0xBFC00180u
#define CP0_STATUS 12u
#define CP0_CAUSE 13u
#define CP0_EPC 14u

// Where the window stood when it ended. File-scope because the memory callbacks discover it while
// `oracle_run` reports it, and Mednafen's callback signatures have nowhere to thread it through.
static OracleStop s_stop = ORACLE_STOP_NONE;
static uint32_t s_stop_addr = 0;
static OracleStop s_taint = ORACLE_STOP_NONE;
static uint32_t s_taint_addr = 0;
static uint32_t s_device_writes = 0;

// Whether THIS file has brought the oracle up. It cannot be inferred from `PSX_CPU`: cpu.c defines it as
// `PS_CPU *PSX_CPU = &s_cpu;` (line 111), i.e. non-NULL from program start, and `CPU_New` hands back that
// same file-scope instance rather than an allocation. Keying the guard on `PSX_CPU != NULL` made
// `oracle_init` return SUCCESS having allocated no RAM at all, and the first `memcpy` into main RAM then
// segfaulted — measured 2026-08-13. Lifecycle state belongs to whoever owns the lifecycle.
static int s_up = 0;

// The core's own cycle count, accumulated across every run/step since the last load. It is the ONE clock
// the plan's DETERMINISM section requires: nothing else advances it, because `PSX_SetEventNT` refuses to
// schedule and no timer or DMA is built into this library at all.
static int32_t s_ts = 0;

const char *oracle_stop_name(OracleStop s) {
  switch (s) {
  case ORACLE_STOP_NONE:
    return "NOT RUN";
  case ORACLE_STOP_BUDGET:
    return "cycle budget consumed (clean end of window)";
  case ORACLE_STOP_HARDWARE:
    return "hardware register touched (window ended here)";
  case ORACLE_STOP_EVENT:
    return "scheduled event came due";
  }
  return "UNKNOWN";
}

// ── the memory bus ────────────────────────────────────────────────────────────────────────────────
// Main RAM is mirrored at 0x00000000 (KUSEG), 0x80000000 (KSEG0) and 0xA0000000 (KSEG1); the scratchpad
// is not mirrored. Masking the segment bits is what the framework's own physical-address handling does.
static inline int in_main_ram(uint32_t a, uint32_t *off) {
  const uint32_t phys = a & 0x1FFFFFFFu;
  if (phys < RAM_WINDOW) {
    *off = phys & RAM_MASK;
    return 1;
  } // 2 MB mirrored 4x, as the reference does
  return 0;
}
static inline int in_scratch(uint32_t a, uint32_t *off) {
  const uint32_t phys = a & 0x1FFFFFFFu, base = SPAD_BASE & 0x1FFFFFFFu;
  if (phys >= base && phys < base + SPAD_SIZE) {
    *off = phys - base;
    return 1;
  }
  return 0;
}
static inline int in_irq(uint32_t a) {
  const uint32_t phys = a & 0x1FFFFFFFu;
  return phys >= IRQ_BASE && phys < IRQ_END;
}
static inline int in_dma_dpcr(uint32_t a) {
  const uint32_t phys = a & 0x1FFFFFFFu;
  return phys >= DMA_DPCR_BASE && phys < DMA_DPCR_END;
}
static inline uint8_t *ram(void) {
  return (uint8_t *)MultiAccessSizeMem_get_data32(MainRAM);
}
static inline uint8_t *spad(void) {
  return (uint8_t *)MultiAccessSizeMem_get_data32(ScratchRAM);
}

// An unsupported device access ends the window. Recorded AND announced, and the CPU is told to stop, so
// the caller sees the exact instruction rather than a run that carried on over garbage.
static void hw_access(uint32_t addr, int is_write, int bits) {
  if (s_stop == ORACLE_STOP_NONE || s_stop == ORACLE_STOP_BUDGET) {
    s_stop = ORACLE_STOP_HARDWARE;
    s_stop_addr = addr;
  }
  s_taint = ORACLE_STOP_HARDWARE;
  s_taint_addr = addr;
  fprintf(stderr,
          "oracle: UNSUPPORTED HARDWARE %s%d at 0x%08X.\n"
          "        Only main RAM, scratchpad, I_STAT/I_MASK, and DPCR are modeled. The window is\n"
          "        OVER at this instruction; a comparison past it would invent\n"
          "        device behavior. Reported instead of returning 0, because a zero here cannot be\n"
          "        distinguished from a device that really held zero.\n",
          is_write ? "WRITE" : "READ",
          bits,
          addr);
  cpu_next_event_ts = 0; // end the timeslice at the next check rather than executing on
}

uint8_t MDFN_FASTCALL PSX_MemRead8(int32_t *ts, uint32_t A) {
  uint32_t o;
  if (in_main_ram(A, &o)) {
    return ram()[o];
  }
  if (in_scratch(A, &o)) {
    return spad()[o];
  }
  if (in_irq(A)) {
    (*ts)++;
    return (uint8_t)IRQ_Read(A);
  }
  if (in_dma_dpcr(A)) {
    uint32_t value = 0;
    (*ts)++;
    if (DMA_DPCR_Read(A, &value)) {
      return (uint8_t)value;
    }
  }
  hw_access(A, 0, 8);
  return 0;
}
uint16_t MDFN_FASTCALL PSX_MemRead16(int32_t *ts, uint32_t A) {
  uint32_t o;
  uint16_t v;
  if (in_main_ram(A, &o)) {
    memcpy(&v, ram() + o, 2);
    return v;
  }
  if (in_scratch(A, &o)) {
    memcpy(&v, spad() + o, 2);
    return v;
  }
  if (in_irq(A)) {
    (*ts)++;
    return (uint16_t)IRQ_Read(A);
  }
  if (in_dma_dpcr(A)) {
    uint32_t value = 0;
    (*ts)++;
    if (DMA_DPCR_Read(A, &value)) {
      return (uint16_t)value;
    }
  }
  hw_access(A, 0, 16);
  return 0;
}
uint32_t MDFN_FASTCALL PSX_MemRead32(int32_t *ts, uint32_t A) {
  uint32_t o, v;
  if (in_main_ram(A, &o)) {
    memcpy(&v, ram() + o, 4);
    return v;
  }
  if (in_scratch(A, &o)) {
    memcpy(&v, spad() + o, 4);
    return v;
  }
  if (in_irq(A)) {
    (*ts)++;
    return IRQ_Read(A);
  }
  if (in_dma_dpcr(A)) {
    uint32_t value = 0;
    (*ts)++;
    if (DMA_DPCR_Read(A, &value)) {
      return value;
    }
  }
  hw_access(A, 0, 32);
  return 0;
}
// 24-bit access serves the unaligned-load path; it is a 32-bit fetch at the same address.
uint32_t MDFN_FASTCALL PSX_MemRead24(int32_t *ts, uint32_t A) {
  return PSX_MemRead32(ts, A);
}

void MDFN_FASTCALL PSX_MemWrite8(int32_t ts, uint32_t A, uint32_t V) {
  uint32_t o;
  (void)ts;
  if (in_main_ram(A, &o)) {
    ram()[o] = (uint8_t)V;
    return;
  }
  if (in_scratch(A, &o)) {
    spad()[o] = (uint8_t)V;
    return;
  }
  if (in_irq(A)) {
    IRQ_Write(A, V);
    s_device_writes |= ((A & 0x1FFFFFFFu) < 0x1F801074u) ? ORACLE_DEVICE_I_STAT : ORACLE_DEVICE_I_MASK;
    return;
  }
  if (in_dma_dpcr(A)) {
    if (!DMA_DPCR_Write(A, V)) {
      hw_access(A, 1, 8);
    } else {
      s_device_writes |= ORACLE_DEVICE_DPCR;
    }
    return;
  }
  hw_access(A, 1, 8);
}
void MDFN_FASTCALL PSX_MemWrite16(int32_t ts, uint32_t A, uint32_t V) {
  uint32_t o;
  uint16_t v = (uint16_t)V;
  (void)ts;
  if (in_main_ram(A, &o)) {
    memcpy(ram() + o, &v, 2);
    return;
  }
  if (in_scratch(A, &o)) {
    memcpy(spad() + o, &v, 2);
    return;
  }
  if (in_irq(A)) {
    IRQ_Write(A, V);
    s_device_writes |= ((A & 0x1FFFFFFFu) < 0x1F801074u) ? ORACLE_DEVICE_I_STAT : ORACLE_DEVICE_I_MASK;
    return;
  }
  if (in_dma_dpcr(A)) {
    if (!DMA_DPCR_Write(A, V)) {
      hw_access(A, 1, 16);
    } else {
      s_device_writes |= ORACLE_DEVICE_DPCR;
    }
    return;
  }
  hw_access(A, 1, 16);
}
void MDFN_FASTCALL PSX_MemWrite32(int32_t ts, uint32_t A, uint32_t V) {
  uint32_t o;
  (void)ts;
  if (in_main_ram(A, &o)) {
    memcpy(ram() + o, &V, 4);
    return;
  }
  if (in_scratch(A, &o)) {
    memcpy(spad() + o, &V, 4);
    return;
  }
  if (in_irq(A)) {
    IRQ_Write(A, V);
    s_device_writes |= ((A & 0x1FFFFFFFu) < 0x1F801074u) ? ORACLE_DEVICE_I_STAT : ORACLE_DEVICE_I_MASK;
    return;
  }
  if (in_dma_dpcr(A)) {
    if (!DMA_DPCR_Write(A, V)) {
      hw_access(A, 1, 32);
    } else {
      s_device_writes |= ORACLE_DEVICE_DPCR;
    }
    return;
  }
  hw_access(A, 1, 32);
}
void MDFN_FASTCALL PSX_MemWrite24(int32_t ts, uint32_t A, uint32_t V) {
  PSX_MemWrite32(ts, A, V);
}

// ── the event scheduler ───────────────────────────────────────────────────────────────────────────
// `PSX_EventHandler` is how a host ends a timeslice: CPU_Run's outer loop is
// `do { ... } while (PSX_EventHandler(timestamp))`, so returning false stops the run. Returning false
// here is therefore NOT a stub pretending to work — it is the documented lever. And the only thing that
// can bring the timestamp up to `cpu_next_event_ts` is the budget `oracle_run` set, because
// `PSX_SetEventNT` below refuses to schedule anything else. That is what makes "no counter influenced
// this window" a checked property rather than an assumption.
bool MDFN_FASTCALL PSX_EventHandler(const int32_t timestamp) {
  (void)timestamp;
  if (s_stop == ORACLE_STOP_NONE) {
    s_stop = ORACLE_STOP_BUDGET;
  }
  return false;
}

void PSX_SetEventNT(const int type, const int32_t next_timestamp) {
  // Deliberately ignored, and LOUD about it: if a subsystem tries to schedule work, the window is no
  // longer the counter-free straight line milestone 1 is defined as, and silently dropping the event
  // would leave a timing difference to be discovered at milestone 4 instead.
  fprintf(stderr,
          "oracle: a subsystem scheduled event type %d at t=%d. Milestone 1's window is defined as\n"
          "        touching no counters, so this is dropped and flagged rather than honoured. If it\n"
          "        fires for real, the window needs shortening or a device-aware oracle.\n"
          "        See docs/oracle.md.\n",
          type,
          next_timestamp);
  if (s_taint == ORACLE_STOP_NONE) {
    s_taint = ORACLE_STOP_EVENT;
    s_taint_addr = 0;
  }
  s_stop = ORACLE_STOP_EVENT;
  cpu_next_event_ts = 0;
}

// `MDFNSS_StateAction` reaches the CPU only from `CPU_StateAction`, which milestone 1 never calls: the
// starting state comes from injecting an executable, not from thawing a savestate. It aborts rather than
// returning success, because a savestate that silently did nothing would leave a zeroed core that would
// still step, and still compare, and mean nothing.
int MDFNSS_StateAction(void *st, int load, int data_only, SFORMAT *sf, const char *name) {
  (void)st;
  (void)load;
  (void)data_only;
  (void)sf;
  fprintf(stderr,
          "oracle: REACHED MDFNSS_StateAction(\"%s\") — savestate serialisation. Milestone 1 neither\n"
          "        saves nor loads state; its start point is an injected executable. Aborting rather\n"
          "        than reporting success.\n",
          name ? name : "(unnamed)");
  fflush(stderr);
  abort();
}

// ── lifecycle ─────────────────────────────────────────────────────────────────────────────────────
int oracle_init(void) {
  if (s_up) {
    return 1; // idempotent by design
  }

  MainRAM = MultiAccessSizeMem_New(RAM_SIZE);
  ScratchRAM = MultiAccessSizeMem_New(SPAD_SIZE);
  if (!MainRAM || !ScratchRAM) {
    fprintf(stderr,
            "oracle: REFUSING — could not allocate main RAM (%u B) + scratchpad (%u B). "
            "Nothing was initialised; do not step.\n",
            RAM_SIZE,
            SPAD_SIZE);
    if (MainRAM) {
      MultiAccessSizeMem_Free(MainRAM);
      MainRAM = NULL;
    }
    if (ScratchRAM) {
      MultiAccessSizeMem_Free(ScratchRAM);
      ScratchRAM = NULL;
    }
    return 0;
  }
  memset(ram(), 0, RAM_SIZE);
  memset(spad(), 0, SPAD_SIZE);

  PSX_CPU = CPU_New(); // returns cpu.c's single file-scope PS_CPU; nothing to free later
  if (!PSX_CPU) {
    fprintf(stderr, "oracle: REFUSING — CPU_New() returned NULL.\n");
    return 0;
  }
  CPU_Power(PSX_CPU);
  IRQ_Power();
  DMA_DPCR_Power();

  // Instruction fetch does NOT go through PSX_MemRead32: cpu.c reads opcodes straight out of `FastMap`
  // (lines 794 and 810), so a core with an unpopulated FastMap fetches from `DummyPage` and executes
  // zeros no matter how correct the memory callbacks are. Mirrors copied from libretro.c:2720-2725.
  for (uint32_t ma = 0; ma < RAM_WINDOW; ma += RAM_SIZE) {
    CPU_SetFastMap(PSX_CPU, ram(), 0x00000000u + ma, RAM_SIZE);
    CPU_SetFastMap(PSX_CPU, ram(), 0x80000000u + ma, RAM_SIZE);
    CPU_SetFastMap(PSX_CPU, ram(), 0xA0000000u + ma, RAM_SIZE);
  }
  // BIOSROM is deliberately NOT mapped: an executable that jumps to 0xBFC00000 must fail visibly rather
  // than quietly run an OpenBIOS our own port never executes.

  s_stop = ORACLE_STOP_NONE;
  s_stop_addr = 0;
  s_taint = ORACLE_STOP_NONE;
  s_taint_addr = 0;
  s_device_writes = 0;
  s_ts = 0;
  s_up = 1;
  return 1;
}

void oracle_teardown(void) {
  // `PSX_CPU` is deliberately left pointing at cpu.c's own instance: `CPU_Destroy` frees nothing (it only
  // shuts down lightrec), so nulling it would be a lie about ownership. The next `oracle_init` re-powers
  // and re-maps that same instance, which is what makes back-to-back windows independent.
  if (s_up) {
    CPU_Destroy(PSX_CPU);
  }
  if (MainRAM) {
    MultiAccessSizeMem_Free(MainRAM);
    MainRAM = NULL;
  }
  if (ScratchRAM) {
    MultiAccessSizeMem_Free(ScratchRAM);
    ScratchRAM = NULL;
  }
  s_up = 0;
}

int oracle_load_exe(const void *image, uint32_t len, uint32_t t_addr, uint32_t pc, uint32_t gp, uint32_t sp) {
  if (!s_up) {
    fprintf(stderr, "oracle: REFUSING to load — oracle_init() has not succeeded.\n");
    return 0;
  }
  uint32_t off;
  if (!in_main_ram(t_addr, &off) || (uint64_t)off + len > RAM_SIZE) {
    fprintf(stderr,
            "oracle: REFUSING to load — t_addr 0x%08X + %u bytes does not fit in %u B of main "
            "RAM. Nothing was copied.\n",
            t_addr,
            len,
            RAM_SIZE);
    return 0;
  }
  // Loading an executable is a fresh-console boundary, not an overlay operation. Re-power every state
  // owner and clear both memories before copying so a second load on one initialized oracle cannot
  // inherit GPRs, IRQ/DPCR state, scratch contents, or bytes outside the new image.
  CPU_Power(PSX_CPU);
  IRQ_Power();
  DMA_DPCR_Power();
  memset(ram(), 0, RAM_SIZE);
  memset(spad(), 0, SPAD_SIZE);
  memcpy(ram() + off, image, len);

  uint32_t *r = CPU_GPR(PSX_CPU);
  r[28] = gp; // $gp — the executable's own global pointer, from its PS-X EXE header
  r[29] = sp; // $sp
  r[30] = sp; // $fp starts at $sp, as every Sony crt0 leaves it
  PSX_CPU->BACKED_PC = pc;
  PSX_CPU->BACKED_new_PC = pc + 4; // no branch pending at entry

  s_stop = ORACLE_STOP_NONE;
  s_stop_addr = 0;
  s_taint = ORACLE_STOP_NONE;
  s_taint_addr = 0;
  s_device_writes = 0;
  s_ts = 0; // a fresh image starts a fresh clock; carrying one over would make two runs of
            // the same fixture report different cycle positions for the same instruction
  return 1;
}

// One slice of execution: run from the accumulated timestamp until `budget` more cycles have passed.
// Shared by `oracle_run` and `oracle_step` so a whole window and a single instruction cannot drift apart
// in how they treat the clock — the budget is the only difference between them.
static OracleStop oracle_slice(int32_t budget) {
  if (s_taint != ORACLE_STOP_NONE) {
    s_stop = s_taint;
    s_stop_addr = s_taint_addr;
    return s_stop;
  }
  s_stop = ORACLE_STOP_NONE;
  s_stop_addr = 0;
  cpu_next_event_ts = s_ts + budget;
  s_ts = CPU_Run(PSX_CPU, s_ts);
  if (s_stop == ORACLE_STOP_NONE) {
    s_stop = ORACLE_STOP_BUDGET;
  }
  return s_stop;
}

OracleStop oracle_run(int32_t cycles) {
  if (!s_up) {
    fprintf(stderr, "oracle: REFUSING to run — no CPU. oracle_init() has not succeeded.\n");
    return ORACLE_STOP_NONE;
  }
  return oracle_slice(cycles);
}

OracleStop oracle_step(void) {
  if (!s_up) {
    fprintf(stderr, "oracle: REFUSING to step — no CPU. oracle_init() has not succeeded.\n");
    return ORACLE_STOP_NONE;
  }
  // A 1-cycle budget. The core may spend more than one cycle on one instruction (a multiply, a GTE op and
  // a load stall all cost several), so a step is "at least one instruction", not "exactly one cycle" —
  // stated because a caller counting steps as cycles would mis-locate every divergence it found.
  return oracle_slice(1);
}

int oracle_resume_call_return(uint32_t expected_target,
                              uint32_t expected_return_pc,
                              uint32_t return_v0,
                              uint32_t return_v1) {
  if (!s_up) {
    fprintf(stderr, "oracle: REFUSING modeled call return — oracle_init() has not succeeded.\n");
    return 0;
  }
  if (s_taint != ORACLE_STOP_NONE) {
    fprintf(stderr,
            "oracle: REFUSING modeled call return — the window is tainted by %s at 0x%08X.\n",
            oracle_stop_name(s_taint),
            s_taint_addr);
    return 0;
  }

  uint32_t *r = CPU_GPR(PSX_CPU);
  if (PSX_CPU->BACKED_PC != expected_target) {
    fprintf(stderr,
            "oracle: REFUSING modeled call return — current target is 0x%08X, expected 0x%08X.\n",
            PSX_CPU->BACKED_PC,
            expected_target);
    return 0;
  }
  if (r[31] != expected_return_pc) {
    fprintf(stderr,
            "oracle: REFUSING modeled call return — current $ra is 0x%08X, expected 0x%08X.\n",
            r[31],
            expected_return_pc);
    return 0;
  }
  // GPR_full[34] is the core's load-delay sink; BACKED_LDWhich==34 means no delayed load is pending.
  // An external leaf would give a delay-slot load time to commit, but an instant modeled return cannot
  // reproduce that passage of instructions. Refuse rather than making the value visible one caller
  // instruction late.
  if (PSX_CPU->BACKED_LDWhich != 34u) {
    fprintf(stderr,
            "oracle: REFUSING modeled call return — delayed load into register %u is still pending.\n",
            PSX_CPU->BACKED_LDWhich);
    return 0;
  }

  uint32_t unused_offset = 0;
  if ((expected_return_pc & 3u) != 0 || !in_main_ram(expected_return_pc, &unused_offset)) {
    fprintf(stderr,
            "oracle: REFUSING modeled call return — $ra 0x%08X is not an aligned main-RAM address.\n",
            expected_return_pc);
    return 0;
  }

  r[2] = return_v0;
  r[3] = return_v1;
  PSX_CPU->BACKED_PC = expected_return_pc;
  PSX_CPU->BACKED_new_PC = expected_return_pc + 4u;
  s_stop = ORACLE_STOP_NONE;
  s_stop_addr = 0;
  return 1;
}

int oracle_resume_syscall_return(uint32_t expected_selector, uint32_t return_v0, uint32_t return_v1) {
  if (!s_up) {
    fprintf(stderr, "oracle: REFUSING syscall return — oracle_init() has not succeeded.\n");
    return 0;
  }
  if (s_taint != ORACLE_STOP_NONE) {
    fprintf(stderr,
            "oracle: REFUSING syscall return — the window is tainted by %s at 0x%08X.\n",
            oracle_stop_name(s_taint),
            s_taint_addr);
    return 0;
  }
  uint32_t *r = CPU_GPR(PSX_CPU);
  const uint32_t cause = CPU_GetCOP0(CP0_CAUSE);
  const uint32_t epc = CPU_GetCOP0(CP0_EPC);
  uint32_t epc_offset = 0;
  uint32_t instruction = 0;
  if (PSX_CPU->BACKED_PC != SYSCALL_VECTOR || ((cause >> 2) & 0x1Fu) != 8u || (cause >> 31) != 0u ||
      r[4] != expected_selector || PSX_CPU->BACKED_LDWhich != 34u || (epc & 3u) != 0u ||
      !in_main_ram(epc, &epc_offset)) {
    fprintf(stderr,
            "oracle: REFUSING syscall return — pc=0x%08X cause=0x%08X epc=0x%08X "
            "selector $a0=0x%08X (expected 0x%08X) ld=%u do not match the requested "
            "non-delay-slot syscall boundary.\n",
            PSX_CPU->BACKED_PC,
            cause,
            epc,
            r[4],
            expected_selector,
            PSX_CPU->BACKED_LDWhich);
    return 0;
  }
  memcpy(&instruction, ram() + epc_offset, sizeof(instruction));
  if ((instruction & 0xFC00003Fu) != 0x0000000Cu) {
    fprintf(stderr,
            "oracle: REFUSING syscall return — EPC 0x%08X holds 0x%08X, not a syscall instruction.\n",
            epc,
            instruction);
    return 0;
  }
  const uint32_t status = CPU_GetCOP0(CP0_STATUS);
  CPU_SetCOP0(CP0_STATUS, (status & ~0x0Fu) | ((status >> 2) & 0x0Fu));
  r[2] = return_v0;
  r[3] = return_v1;
  PSX_CPU->BACKED_PC = epc + 4u;
  PSX_CPU->BACKED_new_PC = epc + 8u;
  s_stop = ORACLE_STOP_NONE;
  s_stop_addr = 0;
  return 1;
}

int32_t oracle_timestamp(void) {
  return s_ts;
}

void oracle_capture(OracleState *out) {
  memset(out, 0, sizeof(*out));
  if (!s_up) {
    out->stop = ORACLE_STOP_NONE;
    return;
  }
  const uint32_t *r = CPU_GPR(PSX_CPU);
  memcpy(out->gpr, r, 32 * sizeof(uint32_t));
  out->lo = r[32];
  out->hi = r[33];
  out->pc = PSX_CPU->BACKED_PC;
  out->next_pc = PSX_CPU->BACKED_new_PC;
  out->timestamp = s_ts; // cycles CONSUMED, not the budget — reporting the budget would make every
                         // capture claim the window ran to completion even when hardware cut it short
  out->stop = s_stop;
  out->stop_addr = s_stop_addr;
  out->cp0_status = CPU_GetCOP0(CP0_STATUS);
  out->cp0_cause = CPU_GetCOP0(CP0_CAUSE);
  out->cp0_epc = CPU_GetCOP0(CP0_EPC);
}

int oracle_capture_devices(OracleDeviceState *out) {
  memset(out, 0, sizeof(*out));
  if (!s_up || s_taint != ORACLE_STOP_NONE) {
    return 0;
  }
  uint32_t dpcr = 0;
  if (!DMA_DPCR_Read(DMA_DPCR_BASE, &dpcr)) {
    return 0;
  }
  out->i_stat = IRQ_Read(IRQ_BASE) & 0x7FFu;
  out->i_mask = IRQ_Read(IRQ_BASE + 4u) & 0x7FFu;
  out->dpcr = dpcr;
  out->valid = ORACLE_DEVICE_ALL;
  out->writes = s_device_writes;
  return 1;
}

uint8_t *oracle_main_ram(void) {
  return MainRAM ? ram() : NULL;
}
uint32_t oracle_ram_size(void) {
  return RAM_SIZE;
}

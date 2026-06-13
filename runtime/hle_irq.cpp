// psxport HLE BIOS — Stage 3: IRQ / exception delivery.
//
// With the pure-native HLE BIOS (PSXPORT_HLE_BIOS=1) there is no MIPS BIOS
// exception handler installed in RAM (0x80000080) — the real BIOS would copy a
// small dispatcher there during boot, which we skipped. So when a hardware IRQ
// becomes pending and the game has enabled interrupts (COP0 SR IEc + the IM
// bits), beetle's interpreter vectors the CPU to the general-exception entry
// (0x80000080, or 0xBFC00180 while SR bit22 BEV is still set — which it is,
// since CPU_Power sets BEV and our HLE boot never ran the BIOS code that clears
// it). Both vectors are empty/garbage under HLE, so the CPU derails.
//
// This file installs PC hooks at BOTH vectors and natively emulates what the
// BIOS exception handler does for a Tomba2-style game:
//   1. Read COP0 CAUSE. ExcCode (bits [6:2]):
//        0  = interrupt  -> service it (below)
//        8  = SYSCALL    -> the game's own a0-dispatched syscalls; the only one
//                           a normal game issues is EnterCriticalSection /
//                           ExitCriticalSection (sets/clears SR IEc). Honor a0.
//        9  = breakpoint -> ignore (advance past).
//      others           -> log once (should not happen in normal boot).
//   2. For an interrupt: deliver the BIOS events the game OpenEvent()'d for the
//      pending IRQ classes (vblank / cdrom / dma), then invoke the game's
//      registered interrupt handler (B0:0x19 HookEntryInt) as a subroutine. The
//      game handler reads I_STAT, services CD/vblank, acks I_STAT, and calls
//      DeliverEvent itself. We trampoline: set $ra to a sentinel, jump to the
//      handler, and a hook on the sentinel performs the return-from-exception
//      (pop SR, restore PC = EPC / branch target).
//   3. Return-from-exception (RFE): SR = (SR & ~0xF) | ((SR>>2)&0xF); PC = EPC,
//      honoring the branch-delay case (EPC was adjusted -4 and CP0.TAR holds the
//      branch target — beetle records that in CAUSE bit31 BD).
//
// All BIOS/IRQ policy lives here; beetle exposes only generic COP0/IRQ
// accessors (psxport_cpu_cop0/_set_cop0, psxport_irq_status/_mask/_ack).

#include "psxport_hooks.h"
#include "hle_bios.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

// COP0 register indices (match cpu.h CP0REG_*).
enum { CP0_TAR = 6, CP0_SR = 12, CP0_CAUSE = 13, CP0_EPC = 14 };

// IRQ controller bits (irq.h).
enum { IRQ_VBLANK = 0, IRQ_GPU = 1, IRQ_CD = 2, IRQ_DMA = 3,
       IRQ_TIMER0 = 4, IRQ_TIMER1 = 5, IRQ_TIMER2 = 6, IRQ_SIO = 7 };

// BIOS event classes per pending-IRQ bit (OpenBIOS EVENT_*: F0000001=VBLANK,
// F0000002=GPU, F0000003=CDROM, F0000004=DMA). The AUTHORITATIVE delivery is the
// game's own root dispatcher (invoked below), which reads I_STAT and DeliverEvents
// in the correct order. We additionally flag any matching open+enabled EvCB fired
// (spec mask 0xFFFFFFFF = any spec of the class) as a belt-and-suspenders measure
// for handlers that TestEvent() before the dispatcher clears I_STAT; this only
// sets the fired flag (never invokes callbacks — the dispatcher owns that).
struct EventClass { int irq_bit; uint32_t ev_class; uint32_t spec; };
const EventClass kEventClasses[] = {
  { IRQ_VBLANK, 0xF0000001u, 0xFFFFFFFFu },
  { IRQ_GPU,    0xF0000002u, 0xFFFFFFFFu },
  { IRQ_CD,     0xF0000003u, 0xFFFFFFFFu },
  { IRQ_DMA,    0xF0000004u, 0xFFFFFFFFu },
};

// Sentinel return address used by the trampoline. It is a KSEG0 address that no
// game code occupies; the interpreter will dispatch a PC hook there (our
// ExceptionReturnHook) which performs the actual RFE + resume. We point it at
// the BIOS break-vector page top, unused under HLE.
const uint32_t kSentinelRA = 0x80000040u;

uint8_t* s_ram = nullptr;      // main RAM (set at install)
uint32_t s_dispatcher = 0;     // resolved root IRQ dispatcher (cached)
bool s_in_exception = false;   // re-entrancy guard (we don't nest IRQs)

// TCB (Thread) field offsets — mirror common/kernel/threads.h + vectors.s. The
// register frame ($k0 in vectors.s) is at TCB+8: GPR[i] at +8+i*4, returnPC at
// +8+0x80=+0x88, hi +0x8C, lo +0x90, SR +0x94, Cause +0x98.
enum {
  TCB_GPR = 0x08, TCB_RETPC = 0x88, TCB_HI = 0x8C, TCB_LO = 0x90,
  TCB_SR = 0x94, TCB_CAUSE = 0x98,
};

// Dedicated kernel exception stack (mirrors OpenBIOS g_exceptionStackPtr): the
// handler chain / game dispatcher runs on THIS stack, not the interrupted
// thread's $sp, so a thread switch mid-handler is clean. Placed in BIOS-reserved
// RAM between the TCB array (ends 0x8000C400) and the work area (0x8000E000):
// grows down from 0x8000DFF8 (0x800-word region), clear of both.
const uint32_t kExcStackTop = 0x8000DFF8u;

// Save the live register file (gpr[1..31] + lo/hi) plus returnPC/SR/Cause into
// the given TCB, at the vectors.s offsets. r0 is not stored (always 0).
void tcb_save(uint8_t* ram, uint32_t tcb, const uint32_t* gpr,
              uint32_t return_pc, uint32_t sr, uint32_t cause) {
  for (int i = 1; i < 32; i++) hle_w32(ram, tcb + TCB_GPR + (uint32_t)i * 4, gpr[i]);
  hle_w32(ram, tcb + TCB_GPR, 0);          // r0
  hle_w32(ram, tcb + TCB_LO, gpr[32]);     // LO (gpr[32])
  hle_w32(ram, tcb + TCB_HI, gpr[33]);     // HI (gpr[33])
  hle_w32(ram, tcb + TCB_RETPC, return_pc);
  hle_w32(ram, tcb + TCB_SR, sr);
  hle_w32(ram, tcb + TCB_CAUSE, cause);
}

// Load gpr[1..31] + lo/hi from the TCB into the live register file; return the
// saved returnPC and (via *out_sr) the saved SR. Caller applies SR + resumes PC.
uint32_t tcb_load(uint8_t* ram, uint32_t tcb, uint32_t* gpr, uint32_t* out_sr) {
  for (int i = 1; i < 32; i++) gpr[i] = hle_r32(ram, tcb + TCB_GPR + (uint32_t)i * 4);
  gpr[0] = 0;
  gpr[32] = hle_r32(ram, tcb + TCB_LO);
  gpr[33] = hle_r32(ram, tcb + TCB_HI);
  if (out_sr) *out_sr = hle_r32(ram, tcb + TCB_SR);
  return hle_r32(ram, tcb + TCB_RETPC);
}

// Resolve the game's root IRQ dispatcher from the HookEntryInt ExCB. word[0] is
// the BIOS-vectored entry; scan forward a few instructions for its first `jal`
// (opcode 6 = 0x0C), whose target is the dispatcher. Returns 0 if unresolvable.
uint32_t resolve_dispatcher(const uint8_t* ram, uint32_t hentry) {
  if (!hentry) return 0;
  const uint32_t entry = hle_r32(ram, hentry); // ExCB word[0]
  if (entry < 0x80000000u || (entry & 0x1FFFFFu) >= 0x200000u) return 0;
  for (uint32_t a = entry; a < entry + 0x40; a += 4) {
    const uint32_t op = hle_r32(ram, a);
    if ((op >> 26) == 0x03) { // JAL: target = (PC&0xF0000000) | (imm26<<2)
      return (a & 0xF0000000u) | ((op & 0x03FFFFFFu) << 2);
    }
  }
  return 0;
}
int s_log = 0;                 // PSXPORT_IRQ_LOG: trace every exception
int s_cs_log = 0;              // PSXPORT_CS_LOG: trace Enter/LeaveCriticalSection SR
uint64_t s_irq_count = 0;

void irq_log_init() {
  if (const char* v = getenv("PSXPORT_IRQ_LOG")) s_log = atoi(v);
  if (const char* v = getenv("PSXPORT_CS_LOG")) s_cs_log = atoi(v);
}

// Outermost-IRQ interrupted-frame store. The game's root IRQ dispatcher runs as
// a subroutine and may itself issue syscalls (Enter/ExitCriticalSection) that
// re-enter the exception handler and transiently overwrite the SINGLE current
// TCB slot with the mid-dispatcher frame. If we restored from the TCB at the
// sentinel we'd resume that stale mid-dispatcher frame instead of the
// interrupted thread (verified: garbage resume PC). So for the outermost IRQ we
// stash the interrupted frame here and restore it at the sentinel UNLESS a
// ChangeThread switched process[0].thread (then we load the new thread's TCB).
uint32_t s_outer_gpr[34];   // interrupted GPR[0..31] + LO/HI
uint32_t s_outer_pc = 0;    // interrupted resume PC
uint32_t s_outer_sr = 0;    // interrupted (pushed) SR
uint32_t s_outer_tcb = 0;   // process[0].thread at outermost IRQ entry

// returnFromException (vectors.s): reload the *current* thread's TCB (which a
// ChangeThread inside the handler may have swapped via process[0].thread), apply
// its saved SR, RFE-pop the SR interrupt/KU stack, and resume at its returnPC.
// `gpr` is the live CPU register file. Returns the resume PC (also redirected by
// the caller). This is the single point where SR is popped and s_in_exception is
// cleared, regardless of how many handlers ran.
uint32_t return_from_exception(uint8_t* ram, uint32_t* gpr) {
  const uint32_t tcb = psxport_hle_current_tcb(ram);
  uint32_t sr = 0;
  uint32_t resume_pc;
  if (tcb) {
    resume_pc = tcb_load(ram, tcb, gpr, &sr); // restores GPR[1..31]+lo/hi
  } else {
    // No kernel tables (shouldn't happen with HLE thread model active): fall
    // back to the live SR + current PC so we at least RFE cleanly.
    sr = psxport_cpu_cop0(CP0_SR);
    resume_pc = psxport_cpu_cop0(CP0_EPC);
  }
  // Apply the saved (at-entry, pushed) SR, then RFE-pop it to the resumed state.
  sr = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu);
  psxport_cpu_set_cop0(CP0_SR, sr);
  s_in_exception = false;
  psxport_cpu_set_pc(resume_pc);
  return resume_pc;
}

// Hook fired when the game's root IRQ dispatcher returns to our sentinel $ra.
// This ends the outermost hardware interrupt. The dispatcher clobbered $ra to
// route here and its callees trash caller-saved regs, so the full interrupted
// register file must be restored — but NOT from the (possibly syscall-clobbered)
// TCB. Restore from the host store, unless the dispatcher switched the current
// thread (a StrPlayer ChangeThread), in which case resume the NEW thread's TCB.
int ExceptionReturnHook(uint32_t /*pc*/, uint32_t* gpr, uint32_t* redirect_pc) {
  uint8_t* ram = s_ram;
  const uint32_t cur = psxport_hle_current_tcb(ram);
  uint32_t resume_pc, sr;
  if (cur && cur != s_outer_tcb) {
    // A ChangeThread inside the dispatcher switched threads: resume the new
    // thread from its TCB frame (the StrPlayer coroutine handoff).
    resume_pc = tcb_load(ram, cur, gpr, &sr);
    if (s_log || s_cs_log)
      fprintf(stderr,
              "[irq f%u] handler returned -> SWITCHED tcb=%08X resume=%08X tcbsr=%08X(IEp%u) -> IEc%u\n",
              psxport_frame, cur, resume_pc, sr, (unsigned)((sr >> 2) & 1),
              (unsigned)(((sr >> 2) & 1)));
  } else {
    // No switch: restore the interrupted frame from the host store (the TCB may
    // have been transiently clobbered by a mid-dispatcher syscall).
    for (int i = 1; i < 32; i++) gpr[i] = s_outer_gpr[i];
    gpr[0] = 0; gpr[32] = s_outer_gpr[32]; gpr[33] = s_outer_gpr[33];
    resume_pc = s_outer_pc;
    // SR: the KU/IE 6-bit stack (bits 0..5) comes from the at-entry saved SR and
    // is RFE-popped; the interrupt-MASK bits (IM, bits 8..15) and other control
    // bits must come from the LIVE SR, because the dispatcher legitimately
    // enables/disables IRQ-mask lines (e.g. arming CD/DMA) that must persist
    // across the return. Popping the at-entry SR wholesale reverts those mask
    // changes and starves/storms the IRQ line (verified). So pop only the stack.
    const uint32_t live = psxport_cpu_cop0(CP0_SR);
    const uint32_t popped_stack = ((s_outer_sr >> 2) & 0x0Fu);
    sr = (live & ~0x3Fu) | popped_stack;
    psxport_cpu_set_cop0(CP0_SR, sr);
    s_in_exception = false;
    psxport_cpu_set_pc(resume_pc);
    *redirect_pc = resume_pc;
    if (s_log)
      fprintf(stderr, "[irq f%u #%llu] return -> PC=%08X sp=%08X ra=%08X (host-restore)\n",
              psxport_frame, (unsigned long long)s_irq_count, resume_pc, gpr[HLE_R_SP], gpr[HLE_R_RA]);
    return PSXPORT_HOOK_REDIRECT;
  }
  sr = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu); // RFE pop (thread-switch branch)
  psxport_cpu_set_cop0(CP0_SR, sr);
  s_in_exception = false;
  psxport_cpu_set_pc(resume_pc);
  *redirect_pc = resume_pc;
  return PSXPORT_HOOK_REDIRECT;
}

// Debug probe (PSXPORT_IRQ_LOG=2): logs I_STAT/I_MASK/SR once per call, to see
// whether VBLANK is pending and why the exception isn't taken. Hooked on the
// VSync spin-wait wrapper 0x80017FC4 (journal: spins until frame counter >= a0).
int VsyncProbeHook(uint32_t /*pc*/, uint32_t* /*gpr*/, uint32_t* /*r*/) {
  static int n = 0;
  if (++n <= 16)
    fprintf(stderr, "[irqprobe f%u] I_STAT=%04X I_MASK=%04X SR=%08X CAUSE=%08X\n",
            psxport_frame, psxport_irq_status(), psxport_irq_mask(),
            psxport_cpu_cop0(CP0_SR), psxport_cpu_cop0(CP0_CAUSE));
  return PSXPORT_HOOK_CONTINUE;
}

// The exception vector hook. Installed at 0x80000080 AND 0xBFC00180.
//
// Faithful TCB-based protocol (mirrors OpenBIOS vectors.s exceptionHandler +
// handlers/syscall.c syscallVerifier + vectors.s returnFromException):
//   1. Save the full interrupted register file (+ returnPC, hi/lo, SR, Cause)
//      into the CURRENT thread's TCB (process[0].thread).
//   2. SYSCALL (excode 8): emulate syscallVerifier on the SAVED TCB fields —
//      advance returnPC+4; a0=1/2 toggle the saved SR critical-section bits;
//      a0=3 swaps process[0].thread (cooperative thread switch). Then
//      returnFromException (reload the now-current TCB + RFE).
//   3. Hardware interrupt (excode 0): deliver events, run the game's root
//      dispatcher on the dedicated exception stack with $ra = the sentinel,
//      which calls returnFromException when the handler returns.
int ExceptionHook(uint32_t /*pc*/, uint32_t* gpr, uint32_t* redirect_pc) {
  uint8_t* ram = s_ram;
  if (!ram)
    return PSXPORT_HOOK_CONTINUE;

  const uint32_t cause = psxport_cpu_cop0(CP0_CAUSE);
  const uint32_t excode = (cause >> 2) & 0x1F;
  const uint32_t live_sr = psxport_cpu_cop0(CP0_SR); // at-entry (HW-pushed) SR

  // EPC + branch-delay handling. beetle wrote CP0.EPC = the faulting PC, minus 4
  // (i.e. the BRANCH instruction's address) if the fault was in a branch delay
  // slot (CAUSE bit31 = BD). On RFE we ALWAYS resume at EPC: for a delay-slot
  // exception that re-executes the branch AND its delay slot — exactly as real
  // MIPS does. Resuming at the branch TARGET instead (the old `bd ? TAR : epc`)
  // SKIPS the delay-slot instruction; when that slot is a function epilogue's
  // `addiu $sp,+N` (e.g. `jr $ra` / `addiu $sp,0x28`), the frame is never popped,
  // so $sp leaks by N and the function later reads a stale return slot and jr's
  // to garbage (the f961 derail). EPC re-execution is the faithful, correct path.
  const uint32_t epc = psxport_cpu_cop0(CP0_EPC);
  const bool bd = (cause & 0x80000000u) != 0;
  const uint32_t resume_pc = epc;

  // --- Nested hardware-interrupt guard (BEFORE touching the TCB). -----------
  // On real hardware IEc=0 during the handler, so a 2nd IRQ cannot fire until
  // RFE. Our dispatcher runs as a subroutine; if it briefly re-enables IEc a
  // VBLANK can re-fire and beetle re-vectors here. We must NOT save this nested
  // context into the TCB — that would clobber the outer interrupted thread's
  // saved frame (causing the resume to load garbage). Instead mask it: pop SR
  // but keep IEc=0, and resume the in-progress dispatcher PC unchanged so it
  // runs to completion (acks I_STAT, returns to the sentinel for the single
  // RFE). Syscalls (excode 8) are synchronous and balanced, so they are allowed
  // to nest and save/restore the TCB normally (handled below).
  if (excode == 0 && s_in_exception) {
    uint32_t sr = (live_sr & ~0x0Fu) | ((live_sr >> 2) & 0x0Fu); // RFE pop
    sr &= ~0x1u;                                                 // keep IEc=0
    psxport_cpu_set_cop0(CP0_SR, sr);
    *redirect_pc = resume_pc;
    return PSXPORT_HOOK_REDIRECT;
  }

  // --- Exception entry: save the interrupted context into the current TCB. ---
  // (vectors.s exceptionHandler: $k0 = processes[0].thread + 8; sw all regs.)
  const uint32_t cur_tcb = psxport_hle_current_tcb(ram);
  if (cur_tcb)
    tcb_save(ram, cur_tcb, gpr, resume_pc, live_sr, cause);

  if (excode == 8) {
    // SYSCALL. a0 selects: 0=NoFunction, 1=EnterCriticalSection, 2=Leave-
    // CriticalSection, 3=ChangeThreadSubFunction. syscallVerifier operates on
    // the SAVED register frame, then returnFromException reloads it.
    const uint32_t a0 = gpr[HLE_R_A0];
    if (cur_tcb) {
      // returnPC += 4 (skip the `syscall` instruction). For a syscall in a
      // branch-delay slot the saved returnPC is already TAR (the branch target),
      // and must NOT be advanced — only bump when we saved EPC, not TAR.
      if (!bd) hle_w32(ram, cur_tcb + TCB_RETPC, hle_r32(ram, cur_tcb + TCB_RETPC) + 4);
      uint32_t saved_sr = hle_r32(ram, cur_tcb + TCB_SR);
      // Critical-section bits operate on the pushed SR: 0x404 = IEp(bit2) +
      // IM-IP2(bit10). After returnFromException's RFE-pop, IEp -> IEc, so
      // clearing 0x404 disables interrupts on resume and setting it enables them.
      if (a0 == 1) { // EnterCriticalSection: v0 = were-enabled?; disable
        hle_w32(ram, cur_tcb + TCB_GPR + HLE_R_V0 * 4,
                (saved_sr & 0x404u) == 0x404u ? 1u : 0u);
        hle_w32(ram, cur_tcb + TCB_SR, saved_sr & ~0x404u);
      } else if (a0 == 2) { // LeaveCriticalSection: enable
        hle_w32(ram, cur_tcb + TCB_SR, saved_sr | 0x404u);
      } else if (a0 == 3) { // ChangeThreadSubFunction(a1 = target TCB)
        // Set v0=1 in the OLD thread's frame (what it sees when switched back),
        // then make a1 the current thread; returnFromException loads a1's frame.
        hle_w32(ram, cur_tcb + TCB_GPR + HLE_R_V0 * 4, 1);
        psxport_hle_set_current_tcb(ram, gpr[HLE_R_A1]);
      }
    }
    // RE probe: critical-section IEc tracking. Logs the SR going in (HW-pushed),
    // the TCB saved-SR we leave for the RFE-pop, and the resulting live IEc, so a
    // LeaveCriticalSection that fails to re-enable interrupts is visible directly.
    const uint32_t cs_saved = (cur_tcb && (a0 == 1 || a0 == 2))
                                  ? hle_r32(ram, cur_tcb + TCB_SR) : 0;
    const bool cs_nest = s_in_exception;
    *redirect_pc = return_from_exception(ram, gpr);
    if (s_cs_log && (a0 == 1 || a0 == 2)) {
      const uint32_t outsr = psxport_cpu_cop0(CP0_SR);
      fprintf(stderr,
              "[cs f%u] %s epc=%08X nest=%d livesr=%08X(IEc%u IEp%u) tcbsr=%08X(IEp%u) -> outsr=%08X(IEc%u) cur=%08X\n",
              psxport_frame, a0 == 1 ? "ENTER" : "LEAVE", epc, cs_nest ? 1 : 0,
              live_sr, (unsigned)(live_sr & 1), (unsigned)((live_sr >> 2) & 1),
              cs_saved, (unsigned)((cs_saved >> 2) & 1), outsr, (unsigned)(outsr & 1), cur_tcb);
    }
    if (s_log)
      fprintf(stderr, "[irq f%u] SYSCALL a0=%u epc=%08X cur=%08X new=%08X\n",
              psxport_frame, a0, epc, cur_tcb, psxport_hle_current_tcb(ram));
    return PSXPORT_HOOK_REDIRECT;
  }

  if (excode == 9) { // breakpoint: restore + resume (returnPC already = resume_pc)
    *redirect_pc = return_from_exception(ram, gpr);
    return PSXPORT_HOOK_REDIRECT;
  }

  if (excode != 0) {
    static uint32_t last = 0xFFFFFFFFu;
    if (excode != last) {
      fprintf(stderr, "[irq f%u] UNHANDLED exception code=%u cause=%08X epc=%08X\n",
              psxport_frame, excode, cause, epc);
      last = excode;
    }
    // Best effort: return-from-exception and resume; better than spinning.
    *redirect_pc = return_from_exception(ram, gpr);
    return PSXPORT_HOOK_REDIRECT;
  }

  // --- Hardware interrupt (ExcCode 0, outermost) ----------------------------
  // (The nested-IRQ case is handled at the top, before any TCB save.)
  const uint16_t pending = psxport_irq_status() & psxport_irq_mask();
  s_irq_count++;
  if (s_log && (s_irq_count < 40 || (s_irq_count % 200) == 0))
    fprintf(stderr, "[irq f%u #%llu] INT pending=%04X sr=%08X epc=%08X bd=%d handler=%08X cur=%08X\n",
            psxport_frame, (unsigned long long)s_irq_count, pending,
            live_sr, epc, bd, psxport_hle_int_handler(), cur_tcb);

  // Pre-deliver the well-known BIOS event classes for each pending IRQ so any
  // handler that TestEvent()s before clearing I_STAT sees the event fired.
  for (const EventClass& ec : kEventClasses) {
    if (pending & (1u << ec.irq_bit)) {
      uint32_t funcs[8];
      psxport_hle_deliver_event_funcs(ec.ev_class, ec.spec, funcs, 8);
    }
  }

  // Invoke the game's root IRQ dispatcher as a subroutine on the dedicated
  // exception stack (mirrors vectors.s switching $sp to g_exceptionStackPtr).
  // It reads I_STAT, services the pending sources (bumps the VBLANK frame
  // counter, ACKs I_STAT, DeliverEvents), then returns to our sentinel $ra,
  // which calls returnFromException (reload the current TCB + RFE).
  const uint32_t hentry = psxport_hle_int_handler();
  const uint32_t disp = s_dispatcher ? s_dispatcher
                      : (s_dispatcher = resolve_dispatcher(ram, hentry));
  if (disp) {
    s_in_exception = true;
    // Stash the interrupted frame for the sentinel (see ExceptionReturnHook).
    for (int i = 0; i < 34; i++) s_outer_gpr[i] = gpr[i];
    s_outer_pc  = resume_pc;
    s_outer_sr  = live_sr;
    s_outer_tcb = cur_tcb;
    gpr[HLE_R_RA] = kSentinelRA; // dispatcher returns -> sentinel hook -> RFE
    // NOTE: run on the interrupted thread's $sp (as the baseline did and as the
    // game's root dispatcher expects). vectors.s swaps to g_exceptionStackPtr,
    // but that is for the OpenBIOS handler chain, not this game-supplied root
    // dispatcher — swapping $sp here starved its I_STAT ack (verified IRQ storm).
    if (s_log && s_irq_count < 16)
      fprintf(stderr, "[irq f%u] ENTRY #%llu -> dispatcher %08X (sp=%08X epc=%08X)\n",
              psxport_frame, (unsigned long long)s_irq_count, disp, gpr[HLE_R_SP], resume_pc);
    *redirect_pc = disp;
    return PSXPORT_HOOK_REDIRECT;
  }

  // No usable game handler: return-from-exception so we don't spin in the vector.
  *redirect_pc = return_from_exception(ram, gpr);
  return PSXPORT_HOOK_REDIRECT;
}

} // namespace

static uint32_t s_pending_switch = 0; // ChangeThread (B0 0x10) requested TCB

// Cooperative thread switch for ChangeThread (B0 0x10), called from
// HleSyscallHook when a switch was requested. Save the caller's live context
// into the CURRENT TCB with returnPC = caller_ra (where it resumes when switched
// back), make new_tcb current, then load new_tcb's frame and return its
// returnPC. This is the non-exception twin of the a0=3 syscall path: B0 0x10's
// caller does not get control back until something switches its thread in again.
uint32_t Hle_Irq_ThreadSwitch(uint8_t* ram, uint32_t* gpr,
                              uint32_t new_tcb, uint32_t caller_ra) {
  const uint32_t cur = psxport_hle_current_tcb(ram);
  if (cur) {
    // Save the caller's regs; it resumes at caller_ra with v0=1 (set by the B0
    // 0x10 handler before this) when switched back. We are NOT in an exception
    // here, so the live SR is the normal (un-pushed) form — store it pre-pushed
    // so the eventual returnFromException/switch RFE-pop restores it intact.
    const uint32_t live_sr = psxport_cpu_cop0(CP0_SR);
    const uint32_t pushed = (live_sr & ~0x3Fu) | ((live_sr & 0x0Fu) << 2);
    tcb_save(s_ram, cur, gpr, caller_ra, pushed, 0);
  }
  psxport_hle_set_current_tcb(ram, new_tcb);
  uint32_t sr = 0;
  const uint32_t resume_pc = tcb_load(s_ram, new_tcb, gpr, &sr);
  // new_tcb's SR is stored pushed (from its last save / OpenThread seed); pop it.
  sr = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu);
  psxport_cpu_set_cop0(CP0_SR, sr);
  if (s_log || s_cs_log)
    fprintf(stderr, "[irq f%u] ChangeThread switch cur=%08X -> new=%08X resume=%08X newsr=%08X(IEc%u)\n",
            psxport_frame, cur, new_tcb, resume_pc, sr, (unsigned)(sr & 1));
  return resume_pc;
}

// Invalidate the cached root-IRQ dispatcher. Called when the game re-registers
// its interrupt entry point (B0 0x19 HookEntryInt) — e.g. after A0(0x51)
// LoadExec swaps in a new EXE (MAIN.EXE) with a different dispatcher. Without
// this, the next IRQ would invoke the PREVIOUS EXE's cached dispatcher
// (0x800182D8), whose code/callback tables the new EXE has overwritten, and
// derail (verified: RI at 0x800183AC right after LoadExec). Forcing a re-resolve
// from the new HookEntryInt ExCB picks up the new EXE's dispatcher.
extern "C" void psxport_hle_irq_reset_dispatcher(void) { s_dispatcher = 0; }

// --- ChangeThread request plumbing (called from hle_kernel.cpp B0 0x10) ------
void psxport_hle_request_thread_switch(uint32_t tcb) { s_pending_switch = tcb; }
uint32_t psxport_hle_take_pending_switch(void) {
  uint32_t t = s_pending_switch; s_pending_switch = 0; return t;
}

// Install the Stage-3 IRQ/exception hooks. Called from main.cpp when the HLE
// BIOS is active.
void Hle_Irq_Install(uint8_t* ram) {
  s_ram = ram;
  irq_log_init();
  // Both general-exception vectors: 0x80000080 (BEV=0) and 0xBFC00180 (BEV=1).
  // Under HLE the boot never cleared BEV, so 0xBFC00180 is the live one; hook
  // both so it works regardless of whether the game clears BEV later. The
  // expected-instr signature check is disabled (0): these are empty/ROM
  // addresses, not overlay-swapped game code, and we always want to catch them.
  psxport_add_hook(0x80000080u, 0, ExceptionHook);
  psxport_add_hook(0xBFC00180u, 0, ExceptionHook);
  psxport_add_hook(kSentinelRA, 0, ExceptionReturnHook);
  if (s_log >= 2) {
    psxport_add_hook(0x80017FC4u, 0, VsyncProbeHook);
    psxport_add_hook(0x800182D8u, 0, VsyncProbeHook); // dispatcher entry
  }
}

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
uint32_t s_saved_gpr[34];      // full interrupted register file (GPR0..31 + LO/HI)

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
uint32_t s_saved_epc = 0;      // captured PC to resume after the handler
int s_log = 0;                 // PSXPORT_IRQ_LOG: trace every exception
uint64_t s_irq_count = 0;

void irq_log_init() {
  if (const char* v = getenv("PSXPORT_IRQ_LOG")) s_log = atoi(v);
}

// Perform the return-from-exception: pop the SR interrupt/KU stack and resume at
// the saved PC. Called from the sentinel hook after the game handler returns.
void do_rfe_resume() {
  uint32_t sr = psxport_cpu_cop0(CP0_SR);
  // "Pop": new IEc/KUc come from IEp/KUp; the rest of the 6-bit stack shifts.
  sr = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu);
  psxport_cpu_set_cop0(CP0_SR, sr);
  s_in_exception = false;
  psxport_cpu_set_pc(s_saved_epc);
}

// Hook fired when the game's interrupt handler returns to our sentinel $ra.
int ExceptionReturnHook(uint32_t /*pc*/, uint32_t* gpr, uint32_t* redirect_pc) {
  if (s_log)
    fprintf(stderr, "[irq f%u] handler returned -> resume EPC=%08X (sr<-pop, regs restored)\n",
            psxport_frame, s_saved_epc);
  // Restore the FULL interrupted register file. We clobbered $ra to route the
  // dispatcher back to this sentinel (without restoring it, resuming at a
  // `jr $ra` would jump to the sentinel and spin). Beyond $ra, the dispatcher's
  // callees freely trash caller-saved regs ($v/$a/$t, LO/HI), which the
  // interrupted instruction stream expects intact across the IRQ — hardware /
  // the BIOS save+restore everything via the process register frame, so we do
  // too. ($sp is left as the dispatcher restored it — it balances its own frame.)
  for (int i = 1; i < 32; i++)
    if (i != HLE_R_SP) gpr[i] = s_saved_gpr[i];
  gpr[32] = s_saved_gpr[32]; // LO
  gpr[33] = s_saved_gpr[33]; // HI
  // We must not let the interpreter execute whatever sits at the sentinel. Pop
  // SR and redirect to the resume PC ourselves.
  do_rfe_resume();
  *redirect_pc = s_saved_epc;
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
int ExceptionHook(uint32_t /*pc*/, uint32_t* gpr, uint32_t* redirect_pc) {
  uint8_t* ram = s_ram;
  if (!ram)
    return PSXPORT_HOOK_CONTINUE;

  const uint32_t cause = psxport_cpu_cop0(CP0_CAUSE);
  const uint32_t excode = (cause >> 2) & 0x1F;

  // EPC + branch-delay handling: beetle already wrote CP0.EPC (PC, minus 4 if in
  // a branch delay slot) and CP0.TAR (the branch target) at exception entry. On
  // resume we go to TAR if we were in a delay slot (CAUSE bit31 = BD), else EPC.
  const uint32_t epc = psxport_cpu_cop0(CP0_EPC);
  const bool bd = (cause & 0x80000000u) != 0;
  const uint32_t resume_pc = bd ? psxport_cpu_cop0(CP0_TAR) : epc;

  if (excode == 8) {
    // SYSCALL. The game's a0 selects: 0=NoFunction, 1=EnterCriticalSection
    // (disable IRQs), 2=ExitCriticalSection (enable IRQs), 3=ChangeThreadSubFn.
    // These manipulate the SR interrupt-enable that our RFE-pop would otherwise
    // restore, so apply them to the *saved/popped* SR view: easiest correct
    // model is to pop SR (return-from-exception) and then set/clear IEc on the
    // resulting SR per a0. EPC for a syscall points AT the `syscall` instr, so
    // resume one instruction past it.
    uint32_t sr = psxport_cpu_cop0(CP0_SR);
    sr = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu); // RFE pop
    const uint32_t a0 = gpr[HLE_R_A0];
    // PSX BIOS uses cop0r12 = 0x401 for "interrupts enabled": IEc (bit0) +
    // IM-IP2 (bit10, the hardware-interrupt mask line). Enter clears both, Exit
    // sets both. Without IM-IP2 set, beetle's CPU_RecalcIPCache never raises a
    // pending interrupt even though I_STAT/I_MASK have it — which is exactly why
    // VBLANK was never delivered (SR was 0x...0001, IM bits all clear).
    if (a0 == 1) { gpr[HLE_R_V0] = (sr & 0x401u) ? 1u : 0u; sr &= ~0x401u; }
    else if (a0 == 2) { sr |= 0x401u; gpr[HLE_R_V0] = 1; }
    psxport_cpu_set_cop0(CP0_SR, sr);
    const uint32_t next = bd ? psxport_cpu_cop0(CP0_TAR) : (epc + 4);
    if (s_log)
      fprintf(stderr, "[irq f%u] SYSCALL a0=%u epc=%08X -> resume %08X\n",
              psxport_frame, a0, epc, next);
    *redirect_pc = next;
    return PSXPORT_HOOK_REDIRECT;
  }

  if (excode == 9) { // breakpoint: skip it
    *redirect_pc = resume_pc;
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
    uint32_t sr = psxport_cpu_cop0(CP0_SR);
    sr = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu);
    psxport_cpu_set_cop0(CP0_SR, sr);
    *redirect_pc = resume_pc;
    return PSXPORT_HOOK_REDIRECT;
  }

  // --- Hardware interrupt (ExcCode 0) ---------------------------------------
  // Re-entrancy guard. On real hardware, taking an exception sets IEc=0, so a
  // second IRQ cannot fire until the handler RFEs. Our handler runs the game
  // dispatcher as a subroutine; if game code inside it briefly re-enables IEc
  // (e.g. an Exit/EnterCriticalSection pair, or a nested event callback), a
  // VBLANK can re-fire mid-dispatch and beetle re-vectors here — EPC pointing
  // INTO the dispatcher. Re-invoking the dispatcher then corrupts its state and
  // spins (the inner call sees the reentrancy flag set, never acks I_STAT, and
  // the resume immediately re-faults). Faithful fix: mask it. Clear IEc and
  // return to the interrupted dispatcher PC WITHOUT re-dispatching, so the
  // in-progress dispatcher runs to completion (it acks I_STAT and returns to
  // our sentinel, which does the single RFE).
  if (s_in_exception) {
    uint32_t sr = psxport_cpu_cop0(CP0_SR);
    sr = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu); // RFE pop
    sr &= ~0x1u;                              // ...but keep IEc=0 (stay masked)
    psxport_cpu_set_cop0(CP0_SR, sr);
    *redirect_pc = resume_pc;
    return PSXPORT_HOOK_REDIRECT;
  }

  const uint16_t pending = psxport_irq_status() & psxport_irq_mask();
  s_irq_count++;
  if (s_log && (s_irq_count < 40 || (s_irq_count % 200) == 0))
    fprintf(stderr, "[irq f%u #%llu] INT pending=%04X sr=%08X epc=%08X bd=%d handler=%08X\n",
            psxport_frame, (unsigned long long)s_irq_count, pending,
            psxport_cpu_cop0(CP0_SR), epc, bd, psxport_hle_int_handler());

  // Pre-deliver the well-known BIOS event classes for each pending IRQ so any
  // handler that TestEvent()s before clearing I_STAT sees the event fired. The
  // mode-0x2000 callbacks are collected but NOT invoked here — the game's own
  // root dispatcher (reached via the registered handler) invokes them in the
  // correct order; invoking them twice would corrupt state. We only flag fired.
  for (const EventClass& ec : kEventClasses) {
    if (pending & (1u << ec.irq_bit)) {
      uint32_t funcs[8];
      psxport_hle_deliver_event_funcs(ec.ev_class, ec.spec, funcs, 8);
    }
  }

  // Invoke the game's root IRQ dispatcher as a subroutine. It reads I_STAT,
  // services the pending sources (incl. bumping the VBLANK frame counter the
  // VSync wait spins on), ACKs I_STAT, and DeliverEvents — a complete service.
  // We trampoline back through the sentinel which performs the RFE+resume.
  //
  // Resolving the dispatcher generically (no magic constant): HookEntryInt's
  // arg (s_int_handler) is a kernel ExCB whose word[0] is the game's
  // BIOS-vectored interrupt entry. That entry is NOT a normal callable function
  // (the real BIOS jr's to it with full context already saved), but it tail-
  // calls the actual dispatcher via a `jal` a few instructions in. We decode
  // that jal target and call the dispatcher directly — it has a normal C
  // prologue/epilogue (`addiu $sp,-N; sw $ra,..($sp); ...; jr $ra`), so calling
  // it with $ra = our sentinel returns cleanly to the RFE epilogue.
  const uint32_t hentry = psxport_hle_int_handler();
  const uint32_t disp = s_dispatcher ? s_dispatcher
                      : (s_dispatcher = resolve_dispatcher(ram, hentry));
  if (disp) {
    s_in_exception = true;
    s_saved_epc = resume_pc;
    for (int i = 0; i < 34; i++) s_saved_gpr[i] = gpr[i]; // full register file
    gpr[HLE_R_RA] = kSentinelRA; // dispatcher returns -> sentinel hook -> RFE
    if (s_log && (s_irq_count < 10))
      fprintf(stderr, "[irq f%u]   -> dispatcher %08X (ra=%08X)\n",
              psxport_frame, disp, kSentinelRA);
    *redirect_pc = disp;
    return PSXPORT_HOOK_REDIRECT;
  }

  // No usable game handler: ack nothing (we don't know which sources are safe to
  // clear), just return-from-exception so we don't spin forever in the vector.
  // If the source isn't acked the IRQ re-fires immediately — but without a
  // handler there's nothing better to do. (Should not happen for Tomba2.)
  uint32_t sr = psxport_cpu_cop0(CP0_SR);
  sr = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu);
  psxport_cpu_set_cop0(CP0_SR, sr);
  *redirect_pc = resume_pc;
  return PSXPORT_HOOK_REDIRECT;
}

} // namespace

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

// Recomp-native HLE BIOS (S2 boot subset). Transcribed from the proven wide60 HLE
// (runtime/hle_kernel.cpp), adapted to operate directly on the Core register file and
// g_ram via the recomp memory accessors. Scope: exactly the A0/B0/C0 calls the boot path
// exercises; extended as the boot/diff harness reveals more. Faithful-first — semantics
// match the wide60 HLE that provably boots Tomba!2; not reimplemented from guesswork.
//
// State (event control blocks, native heap, IRQ / work-area flags) lives on `class Hle`
// in game.h (`c->game->hle`) — per-Core so SBS's two cores keep separate BIOS state. The
// BIOS-call dispatchers below are METHODS on that class; the `rec_syscall` / `rec_break`
// / `rec_dispatch_miss` free entries below are the C-ABI shims the recompiled shards call.
#include "core.h"
#include "game.h"
#include "platform_hle.h"   // class PlatformHle — sync-primitive HLE consulted on a RAM-code miss
#include "memcard.h"        // card_hle_a0 / card_hle_b0 — libcard BIOS dispatch (class Memcard)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "cfg.h"

extern "C" void guest_backtrace_to(Core* c, FILE* out);  // sync_overrides.cpp
extern "C" void guest_find_word_to(Core* c, FILE* out, uint32_t val);  // sync_overrides.cpp

// MIPS o32 register indices (== c->r[]).
enum { A0=4, A1=5, A2=6, A3=7, T1=9, V0=2 };

// ---- Event control blocks (B0:0x07-0x0D) --------------------------------------------
enum { EVCB_MAX = 16 };
static const uint32_t EV_ID_BASE = 0xF1000000u;

int Hle::eventIndex(uint32_t id) const {
  uint32_t idx = id - EV_ID_BASE;
  return (idx < EVCB_MAX && ev[idx].open) ? (int)idx : -1;
}

// Native VBlank delivery (called by the frame tick): mark matching open+enabled slots fired.
void Hle::deliverEvent(uint32_t evClass, uint32_t spec) {
  for (int i = 0; i < EVCB_MAX; i++)
    if (ev[i].open && ev[i].enabled && ev[i].ev_class == evClass && (ev[i].spec & spec))
      ev[i].fired = 1;
}

// ---- Heap (A0:0x33-0x39): native first-fit arena, bookkeeping outside PSX RAM -------
enum { HEAP_MAX_BLOCKS = 4096 };

void Hle::heapInit(uint32_t addr, uint32_t size) {
  heap_base = addr; heap_size = size;
  nblk = 1; blk[0].addr = addr; blk[0].size = size; blk[0].used = 0;
  heap_ok = 1;
}

uint32_t Hle::heapAlloc(uint32_t size) {
  if (!heap_ok || size == 0) return 0;
  size = (size + 7u) & ~7u;
  for (int i = 0; i < nblk; i++) {
    if (blk[i].used || blk[i].size < size) continue;
    if (blk[i].size > size && nblk < HEAP_MAX_BLOCKS) {
      for (int j = nblk; j > i + 1; j--) blk[j] = blk[j - 1];
      blk[i + 1].addr = blk[i].addr + size;
      blk[i + 1].size = blk[i].size - size;
      blk[i + 1].used = 0;
      blk[i].size = size; nblk++;
    }
    blk[i].used = 1; return blk[i].addr;
  }
  return 0;
}

void Hle::heapCoalesce() {
  for (int i = 0; i + 1 < nblk;) {
    if (!blk[i].used && !blk[i + 1].used) {
      blk[i].size += blk[i + 1].size;
      for (int j = i + 1; j + 1 < nblk; j++) blk[j] = blk[j + 1];
      nblk--;
    } else i++;
  }
}

void Hle::heapFree(uint32_t addr) {
  if (!addr) return;
  for (int i = 0; i < nblk; i++)
    if (blk[i].addr == addr && blk[i].used) { blk[i].used = 0; heapCoalesce(); return; }
}

uint32_t Hle::heapBlockSize(uint32_t addr) const {
  for (int i = 0; i < nblk; i++)
    if (blk[i].addr == addr && blk[i].used) return blk[i].size;
  return 0;
}

// ---- Native work area for GetB0Table()/GetC0Table() ---------------------------------
// Tomba2 reads B0table[+0x16C] -> control struct; publish a self-consistent native page.
enum { HLE_B0TABLE = 0x8000F000u, HLE_C0TABLE = 0x8000F800u, HLE_WORK_BASE = 0x8000E000u };
void Hle::workAreaInit() {
  Core* c = &game->core;
  if (work_ok) return;
  work_ok = 1;
  c->mem_w32(HLE_B0TABLE + 0x16Cu, HLE_WORK_BASE);
  c->mem_w32(HLE_C0TABLE + 0, 0x03E00008u);  // jr $ra
  c->mem_w32(HLE_C0TABLE + 4, 0);            // nop
}

// LoadExec (A0:0x51) interceptor: process-scoped hook installed by native_stub at boot.
static void (*s_loadexec_hook)(Core*) = nullptr;

// BIOS threads are implemented natively (per-thread ucontext stacks) in threads.c.
uint32_t thread_open(Core* c);
uint32_t thread_close(Core* c);
void     thread_change(Core* c, uint32_t handle);

// Dispatch one A0/B0/C0 BIOS call. Returns true if handled (c->r[V0] set), false otherwise.
bool Hle::dispatchBios(char table, uint32_t fn) {
  Core* c = &game->core;
  uint32_t a0 = c->r[A0], a1 = c->r[A1], a2 = c->r[A2];
  // `PSXPORT_DEBUG=bios` — EVERY BIOS call the guest makes, handled or not. The existing UNIMPL log
  // only fires for calls that fall through, which answers "what is missing" but not "what does this
  // game actually use" — and that second question is what a port needs when deciding which BIOS
  // subsystem to model. $ra is the caller: the A0/B0/C0 stubs are tail jumps (`li $t2,0xA0; jr $t2`),
  // so it still holds the real call site and links a call back to the library routine that made it.
  if (cfg_dbg("bios"))
    cfg_logf("bios", "%c0:0x%02X(0x%08X, 0x%08X, 0x%08X, 0x%08X) from 0x%08X",
             table, fn, a0, a1, a2, c->r[A3], c->r[31]);
  HleEvCB* s_ev = ev;   // alias so the switch bodies below read tersely
  if (table == 'A') {
    switch (fn) {
      case 0x33: c->r[V0] = heapAlloc(a0); return true;               // malloc
      case 0x34: heapFree(a0); c->r[V0] = 0; return true;              // free
      case 0x37: { uint32_t n = a0 * a1, p = heapAlloc(n);            // calloc
                   if (p) for (uint32_t i = 0; i < n; i++) c->mem_w8(p + i, 0);
                   c->r[V0] = p; return true; }
      case 0x38: { uint32_t old = a0, ns = a1;                           // realloc
                   if (!old) { c->r[V0] = heapAlloc(ns); return true; }
                   if (!ns) { heapFree(old); c->r[V0] = 0; return true; }
                   uint32_t np = heapAlloc(ns), os = heapBlockSize(old);
                   if (np) { uint32_t n = os < ns ? os : ns;
                             for (uint32_t i = 0; i < n; i++) c->mem_w8(np + i, c->mem_r8(old + i));
                             heapFree(old); }
                   c->r[V0] = np; return true; }
      case 0x39: heapInit(a0, a1); c->r[V0] = 0; return true;          // InitHeap
      // --- BIOS libc string/memory leaves -------------------------------------------------------
      // These were absent, and absence here is SILENT DATA LOSS, not a missing feature: an
      // unhandled BIOS call logs UNIMPL, leaves $v0 holding whatever the previous call left there,
      // and — crucially — DOES NOT PERFORM THE WRITE. A guest bzero() that quietly does nothing
      // leaves a structure full of garbage that surfaces arbitrarily far away. Spider-Man's boot
      // calls A(28h) and A(2Ah) twice each; every PSX title that uses the BIOS libc calls them.
      case 0x28:                                                       // bzero(dst, n)
        for (uint32_t i = 0; i < a1; i++) c->mem_w8(a0 + i, 0);
        c->r[V0] = a0; return true;
      case 0x2A:                                                       // memcpy(dst, src, n)
        for (uint32_t i = 0; i < a2; i++) c->mem_w8(a0 + i, c->mem_r8(a1 + i));
        c->r[V0] = a0; return true;
      case 0x2B:                                                       // memset(dst, c, n)
        for (uint32_t i = 0; i < a2; i++) c->mem_w8(a0 + i, (uint8_t)a1);
        c->r[V0] = a0; return true;
      case 0x2C:                                                       // memmove(dst, src, n) —
        if (a0 > a1)                                                   // overlap-correct, unlike 2Ah
          for (uint32_t i = a2; i-- > 0;) c->mem_w8(a0 + i, c->mem_r8(a1 + i));
        else
          for (uint32_t i = 0; i < a2; i++) c->mem_w8(a0 + i, c->mem_r8(a1 + i));
        c->r[V0] = a0; return true;
      // setjmp: fill the guest's jmp_buf with the real callee-saved set and return 0 (the
      // direct-call result). The layout is the BIOS's: ra, sp, fp, s0..s7, gp. Writing it truthfully
      // matters even though longjmp is unsupported below — a guest that INSPECTS the buffer, or
      // that longjmps and is therefore about to abort, should see the state that actually existed.
      case 0x13: {                                                     // setjmp(buf)
        static const int kSave[12] = { 31, 29, 30, 16, 17, 18, 19, 20, 21, 22, 23, 28 };
        for (int i = 0; i < 12; i++) c->mem_w32(a0 + 4u * (uint32_t)i, c->r[kSave[i]]);
        c->r[V0] = 0; return true;
      }
      // longjmp CANNOT be honoured here and must not be faked. Restoring the guest's $sp/$ra does
      // nothing on a static recompile: control lives on the HOST stack, and returning normally
      // would resume the setjmp caller's successor instead of unwinding to it — silently running
      // the error path's continuation with the failed operation's state still live. Stop instead;
      // a guest reaching longjmp is a real event that needs a real mechanism (host setjmp at the
      // dispatch boundary, or a coroutine unwind), not a plausible-looking return.
      case 0x14:                                                       // longjmp(buf, val)
        cfg_loge("hle", "A0:0x14 longjmp(buf=0x%08X, val=%u) — psxport has no guest unwind "
                        "mechanism. Faking a return here would resume the wrong continuation with "
                        "the failed operation's state live. Refusing.", a0, a1);
        abort();
      case 0x44: c->r[V0] = 0; return true;                              // FlushCache (no-op)
      case 0x49: c->r[V0] = 0; return true;                              // GPU_cw (GP0 word — harmless)
      case 0x51: if (s_loadexec_hook) { s_loadexec_hook(c); return true; } return false;  // LoadExec
      case 0x70: c->r[V0] = 0; return true;                              // _bu_init no-op
      case 0x71: c->r[V0] = 0; return true;                              // _96_init no-op
      case 0x72: c->r[V0] = 0; return true;                              // _96_remove no-op
      default: {
        if (card_hle_a0(fn, c)) return true;
        return false;
      }
    }
  }
  if (table == 'B') {
    switch (fn) {
      case 0x07: deliverEvent(a0, a1); c->r[V0] = 0; return true;        // DeliverEvent
      case 0x08: {                                                       // OpenEvent
        for (int i = 0; i < EVCB_MAX; i++)
          if (!s_ev[i].open) {
            s_ev[i].open = 1; s_ev[i].enabled = 0; s_ev[i].fired = 0;
            s_ev[i].ev_class = a0; s_ev[i].spec = a1; s_ev[i].mode = a2; s_ev[i].func = c->r[A3];
            // `PSXPORT_DEBUG=ev` — which events a game opens, and the handle each got. A port must
            // put those CLASSES into GameConfig::irqEventClasses or the per-frame delivery has
            // nothing to deliver and every TestEvent wait spins forever. There was no way to
            // discover a game's classes short of reading its OpenEvent call sites by hand; this
            // prints them from the run. Handle is what TestEvent/WaitEvent are later called with,
            // so it also links a spinning poll back to the event it is waiting on.
            if (cfg_dbg("ev"))
              cfg_logf("ev", "OpenEvent class=0x%08X spec=0x%08X mode=0x%08X handler=0x%08X -> handle=0x%08X",
                       a0, a1, a2, c->r[A3], EV_ID_BASE + (uint32_t)i);
            c->r[V0] = EV_ID_BASE + (uint32_t)i; return true;
          }
        c->r[V0] = 0xFFFFFFFFu; return true;                             // table full
      }
      case 0x09: { int i = eventIndex(a0);                                // CloseEvent
        if (i >= 0) { s_ev[i].open = 0; c->r[V0] = 1; } else c->r[V0] = 0; return true; }
      case 0x0A: { int i = eventIndex(a0);                                // WaitEvent (can't block)
        if (i >= 0) { s_ev[i].fired = 0; c->r[V0] = 1; } else c->r[V0] = 0; return true; }
      case 0x0B: { int i = eventIndex(a0);                                // TestEvent (read+clear)
        if (i >= 0 && s_ev[i].fired) { s_ev[i].fired = 0; c->r[V0] = 1; } else c->r[V0] = 0;
        return true; }
      case 0x0C: { int i = eventIndex(a0);                                // EnableEvent
        if (i >= 0) { s_ev[i].enabled = 1; c->r[V0] = 1; } else c->r[V0] = 0; return true; }
      case 0x0D: { int i = eventIndex(a0);                                // DisableEvent
        if (i >= 0) { s_ev[i].enabled = 0; c->r[V0] = 1; } else c->r[V0] = 0; return true; }
      case 0x12: case 0x13: case 0x14: case 0x15: case 0x16:              // BIOS pad — no-op (native)
        c->r[V0] = 0; return true;
      case 0x19:                                                         // B(19h)
        // NOT "hook a handler function". This is SetCustomExitFromException: $a0 points at a
        // jmp_buf-shaped structure { +0 ra, +4 sp, +8 fp, +0x0C..0x28 s0..s7, +0x2C gp }, and the
        // BIOS exception path — AFTER walking the SysEnqIntRP chains — loads those registers and
        // jumps to `ra` instead of resuming the interrupted context. Dump it: whether `+0` is a
        // function ENTRY decides whether a static recompile can enter it at all. A mid-function `ra`,
        // which a real setjmp would leave, is not an address the substrate can dispatch to.
        if (cfg_dbg("bios")) {
          cfg_logf("bios", "B0:0x19 custom-exit buf=0x%08X: ra=0x%08X sp=0x%08X fp=0x%08X gp=0x%08X",
                   a0, c->mem_r32(a0), c->mem_r32(a0 + 4), c->mem_r32(a0 + 8), c->mem_r32(a0 + 0x2C));
          for (int i = 0; i < 8; i++)
            cfg_logf("bios", "  s%d = 0x%08X", i, c->mem_r32(a0 + 0x0Cu + 4u * (uint32_t)i));
        }
        int_handler = a0; c->r[V0] = 0; return true;
      case 0x35: {                                                       // FileWrite
        uint32_t fd = a0, buf = a1, len = a2;
        if (fd == 1 || fd == 2) for (uint32_t i = 0; i < len; i++) fputc(c->mem_r8(buf + i), stderr);
        c->r[V0] = len; return true;
      }
      case 0x0E: c->r[V0] = thread_open(c); return true;                  // OpenThread
      case 0x0F: c->r[V0] = thread_close(c); return true;                 // CloseThread
      case 0x10: thread_change(c, a0); c->r[V0] = a0; return true;        // ChangeThread
      case 0x4A: c->r[V0] = 0; return true;                              // stopCard / card no-op
      case 0x4B: c->r[V0] = 1; return true;                              // cardInfo -> present
      case 0x56: workAreaInit(); c->r[V0] = HLE_C0TABLE; return true;    // GetC0Table
      case 0x57: workAreaInit(); c->r[V0] = HLE_B0TABLE; return true;    // GetB0Table
      case 0x5B: c->r[V0] = 0; return true;                              // ChangeClearPAD (no-op)
      default: {
        if (card_hle_b0(fn, c)) return true;
        return false;
      }
    }
  }
  if (table == 'C') {
    switch (fn) {
      // SysEnqIntRP / SysDeqIntRP. Still return the element (what callers expect), but LOG it:
      // psxport does not yet dispatch interrupts to guest code (WART-05 in the Spider-Man port), and
      // the first thing a delivery model needs is the guest InterruptElement's field layout, which
      // cannot be read off the SDK header with confidence. Dumping the four words at registration
      // time answers it from the run: whichever slots hold addresses inside the recompiled .text are
      // the handler/verifier pair.
      case 0x02: case 0x03:
        if (cfg_dbg("bios")) {
          const char* which = (fn == 0x02) ? "SysEnqIntRP" : "SysDeqIntRP";
          cfg_logf("bios", "%s prio=%u elem=0x%08X -> [+0]=0x%08X [+4]=0x%08X [+8]=0x%08X [+C]=0x%08X",
                   which, a0, a1, c->mem_r32(a1), c->mem_r32(a1 + 4),
                   c->mem_r32(a1 + 8), c->mem_r32(a1 + 12));
        }
        c->r[V0] = a1; return true;
      case 0x00: case 0x01: case 0x07: case 0x08:                        // kernel-table
      case 0x0A: case 0x0C: case 0x12: case 0x1C:                        // installers + RCnt
        c->r[V0] = 0; return true;
      default: return false;
    }
  }
  return false;
}

// ---- Recomp-ABI C-linkage entry points -----------------------------------------------
// The `syscall` instruction: the kernel op is selected by $a0 (not the code field). Boot uses
// Enter/ExitCriticalSection around setup. Thread ops (a0=3) need the recomp thread model.
void rec_syscall(Core* c, uint32_t code) {
  (void)code;
  int& irq_enabled = c->game->hle.irq_enabled;
  switch (c->r[A0]) {
    case 0: c->r[V0] = 0; break;
    case 1: c->r[V0] = irq_enabled ? 1 : 0; irq_enabled = 0; break;      // EnterCritical
    case 2: irq_enabled = 1; c->r[V0] = 0; break;                        // ExitCritical
    default:
      cfg_logi("syscall", "a0=%u (unhandled kernel op)", c->r[A0]);
      c->r[V0] = 0;
  }
}
void rec_break(Core* c, uint32_t code) {
  cfg_logi("break", "code %u", code);
  (void)c;
}


void rec_dispatch_miss(Core* c, uint32_t addr) {
  uint32_t a = addr & 0x1FFFFFFF;
  char tbl = a == 0xA0 ? 'A' : a == 0xB0 ? 'B' : a == 0xC0 ? 'C' : 0;
  if (tbl) {
    uint32_t fn = c->r[T1] & 0xFF;
    if (c->game->hle.dispatchBios(tbl, fn)) return;
    cfg_logi("hle", "UNIMPL %c0:0x%02X", tbl, fn);
    return;
  }
  // Non-recompiled code in RAM (loaded overlay, the boot stub, or an in-function computed-jump
  // target the recompiler routed here). Skip the low exception/scratchpad region (< 0x10000) which
  // is never a call target. First honor the PLATFORM HLE table: PSX BIOS-library HW-sync leaves
  // (libcd/libetc/libmdec) that busy-spin on an unmodelled IRQ/status bit resolve natively here.
  if (a >= 0x10000 && a < 0x200000) {
    if (auto pf = c->game->platform_hle.lookup(addr | 0x80000000u)) { pf(c); return; }
    // FAIL FAST: the interpreter is gone. Any RAM code that is not a recompiled MAIN function, a
    // native override, or a platform-HLE leaf is a MISS — abort with call site + backtrace.
    if (c->recMissTolerant) { c->recMissed = true; return; }   // TEST: skip oracle-unavailable state
    extern const char* overlay_router_resident_name(Core*, uint32_t);
    const char* resov = overlay_router_resident_name(c, addr);
    if (addr >= 0x80108F9Cu && addr < 0x8018A000u) {       // a MODE/area-slot overlay address
      uint32_t stage = c->mem_r32(0x801fe00cu);
      uint32_t sm = c->mem_r32(0x1f800138u);
      cfg_logi("miss-state", "stage=0x%08X sm=0x%08X sm[48]=%u [4a]=%u [4c]=%u [4e]=%u [50]=%u [52]=%u 1f80019b=%u areaidx(800bf870)=%u sopsig(80109450)=0x%08X 1f800234=%u", stage, sm,
              sm ? c->mem_r16(sm + 0x48) : 0xffff, sm ? c->mem_r16(sm + 0x4a) : 0xffff,
              sm ? c->mem_r16(sm + 0x4c) : 0xffff, sm ? c->mem_r16(sm + 0x4e) : 0xffff,
              sm ? c->mem_r16(sm + 0x50) : 0xffff, sm ? c->mem_r16(sm + 0x52) : 0xffff,
              c->mem_r8(0x1f80019bu), c->mem_r8(0x800bf870u), c->mem_r32(0x80109450u), c->mem_r8(0x1f800234u));
      // Callee-saved regs often still hold the guest caller's locals (e.g. s0 = the object node in
      // the 0x8007D208 SFX-update family) — dump them plus the node fields s0 would imply.
      cfg_logi("miss-regs", "s0=0x%08X s1=0x%08X s2=0x%08X s3=0x%08X", c->r[16], c->r[17], c->r[18], c->r[19]);
      uint32_t s0 = c->r[16];
      if (s0 >= 0x80000000u && s0 < 0x80200000u - 0x60u) {
        cfg_logi("hle", "[miss-node s0] +0x00=0x%08X +0x1c(handler)=0x%08X +0x0d=%u +0x29=%u +0x44=%d +0x46=0x%02X +0x5c=%d", c->mem_r32(s0), c->mem_r32(s0 + 0x1cu), c->mem_r8(s0 + 0x0du), c->mem_r8(s0 + 0x29u),
                (int16_t)c->mem_r16(s0 + 0x44u), c->mem_r8(s0 + 0x46u), (int16_t)c->mem_r16(s0 + 0x5cu));
      }
    }
    cfg_logw("hle", "\n[recomp-MISS %d] no recompiled fn for 0x%08X  (caller ra=0x%08X, a0=0x%08X, c->pc=0x%08X)\n  resident overlay for this slot = %s (if non-A00 but addr is an A00 fn -> stale pointer /\n  wrong overlay resident; if matches but still missed -> function-discovery gap in that overlay)\n  not a recompiled MAIN fn / native override / platform-HLE leaf — likely overlay code or a\n  mid-function coroutine resume. The interpreter is removed; this is fail-fast by design.", c->game->hle.miss_count++, addr, c->r[31], c->r[4], c->pc, resov ? resov : "(addr not in any slot range)");
    guest_backtrace_to(c, stderr);
    guest_find_word_to(c, stderr, addr | 0x80000000u);
    fflush(stderr);
    abort();
  }
  // NULL-CALLBACK NO-OP (addr == 0): a `jalr $zero`-shaped dispatch to a null function pointer. The
  // libsnd SsSeqCalled command dispatcher (FUN_80091460) reads a per-command handler slot
  // (0x80104B90..0x80104BA0) and calls it; an unhandled/meta command leaves that slot null, so the
  // guest dispatches address 0 — a no-op by intent (a call to a null callback does nothing). Both SBS
  // cores reach it identically (0-diff), i.e. it is faithful guest behavior, not a port miss. Return
  // silently: a null callback is a no-op, not a "no recompiled fn" error worth reporting.
  if ((addr & 0x1FFFFFFFu) == 0) return;
  cfg_logi("hle", "[miss %d] addr 0x%08X (no recompiled fn / overlay)  (caller ra=0x%08X c->pc=0x%08X a0=0x%08X)", c->game->hle.miss_count++, addr, c->r[31], c->pc, c->r[4]);
}

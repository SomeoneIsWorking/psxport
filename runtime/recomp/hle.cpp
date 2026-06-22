// Recomp-native HLE BIOS (S2 boot subset). Transcribed from the proven wide60 HLE
// (runtime/hle_kernel.cpp), adapted to operate directly on the Core register file and
// g_ram via the recomp memory accessors. Scope: exactly the A0/B0/C0 calls the boot path
// exercises; extended as the boot/diff harness reveals more. Faithful-first — semantics
// match the wide60 HLE that provably boots Tomba!2; not reimplemented from guesswork.
#include "core.h"
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MIPS o32 register indices (== c->r[]).
enum { A0=4, A1=5, A2=6, A3=7, T1=9, V0=2 };

// ---- Event control blocks (B0:0x07-0x0D) --------------------------------------------
// Transcribed from the proven wide60 HLE (hle_kernel.cpp). SCOPE: no scheduler/IRQ
// delivery in the recomp model, so WaitEvent cannot truly block — it reports the event
// ready and clears `fired`, which terminates the game's wait-then-test loops. A native
// frame source (timing.c) DeliverEvents the VBlank class each VSync so test loops advance.
// EvCB/HeapBlock + their state now live on the instance (HleState in game.h): c->game->hle.*
enum { EVCB_MAX = 16 };
static const uint32_t EV_ID_BASE = 0xF1000000u;
static int ev_index(Core* c, uint32_t id) {
  uint32_t idx = id - EV_ID_BASE;
  return (idx < EVCB_MAX && c->game->hle.ev[idx].open) ? (int)idx : -1;
}
// Native VBlank delivery (called by the frame tick): mark matching open+enabled slots fired.
void hle_deliver_event(Core* c, uint32_t ev_class, uint32_t spec) {
  HleEvCB* ev = c->game->hle.ev;
  for (int i = 0; i < EVCB_MAX; i++)
    if (ev[i].open && ev[i].enabled && ev[i].ev_class == ev_class && (ev[i].spec & spec))
      ev[i].fired = 1;
}

// ---- Heap (A0:0x33-0x39): native first-fit arena, bookkeeping outside PSX RAM -------
enum { HEAP_MAX_BLOCKS = 4096 };

static void heap_init(Core* c, uint32_t addr, uint32_t size) {
  HleState& h = c->game->hle;
  h.heap_base = addr; h.heap_size = size;
  h.nblk = 1; h.blk[0].addr = addr; h.blk[0].size = size; h.blk[0].used = 0;
  h.heap_ok = 1;
}
static uint32_t heap_alloc(Core* c, uint32_t size) {
  HleState& h = c->game->hle;
  if (!h.heap_ok || size == 0) return 0;
  size = (size + 7u) & ~7u;
  for (int i = 0; i < h.nblk; i++) {
    if (h.blk[i].used || h.blk[i].size < size) continue;
    if (h.blk[i].size > size && h.nblk < HEAP_MAX_BLOCKS) {
      for (int j = h.nblk; j > i + 1; j--) h.blk[j] = h.blk[j - 1];
      h.blk[i + 1].addr = h.blk[i].addr + size;
      h.blk[i + 1].size = h.blk[i].size - size;
      h.blk[i + 1].used = 0;
      h.blk[i].size = size; h.nblk++;
    }
    h.blk[i].used = 1; return h.blk[i].addr;
  }
  return 0;
}
static void heap_coalesce(Core* c) {
  HleState& h = c->game->hle;
  for (int i = 0; i + 1 < h.nblk;) {
    if (!h.blk[i].used && !h.blk[i + 1].used) {
      h.blk[i].size += h.blk[i + 1].size;
      for (int j = i + 1; j + 1 < h.nblk; j++) h.blk[j] = h.blk[j + 1];
      h.nblk--;
    } else i++;
  }
}
static void heap_free(Core* c, uint32_t addr) {
  if (!addr) return;
  HleState& h = c->game->hle;
  for (int i = 0; i < h.nblk; i++)
    if (h.blk[i].addr == addr && h.blk[i].used) { h.blk[i].used = 0; heap_coalesce(c); return; }
}
static uint32_t heap_block_size(Core* c, uint32_t addr) {
  HleState& h = c->game->hle;
  for (int i = 0; i < h.nblk; i++)
    if (h.blk[i].addr == addr && h.blk[i].used) return h.blk[i].size;
  return 0;
}

// ---- Native work area for GetB0Table()/GetC0Table() ---------------------------------
// Tomba2 reads B0table[+0x16C] -> control struct, deriving pointers it later uses.
// Publish a self-consistent native page so those reads land on valid memory.
enum { HLE_B0TABLE = 0x8000F000u, HLE_C0TABLE = 0x8000F800u, HLE_WORK_BASE = 0x8000E000u };
static void work_area_init(Core* c) {
  if (c->game->hle.work_ok) return;
  c->game->hle.work_ok = 1;
  c->mem_w32(HLE_B0TABLE + 0x16Cu, HLE_WORK_BASE);
  c->mem_w32(HLE_C0TABLE + 0, 0x03E00008u);  // jr $ra
  c->mem_w32(HLE_C0TABLE + 4, 0);            // nop
}

// B0:0x19 HookEntryInt — game IRQ entry (recorded) now lives on the instance: c->game->hle.int_handler

// BIOS threads are implemented natively (per-thread ucontext stacks) in threads.c.
uint32_t thread_open(Core* c);
uint32_t thread_close(Core* c);
void     thread_change(Core* c, uint32_t handle);

// LoadExec (A0:0x51) interceptor. The disc's boot stub uses LoadExec to load+exec
// cdrom:\MAIN.EXE;1; native_stub installs a hook here that loads MAIN.EXE into RAM and hands
// control to the native MAIN boot (instead of jumping into the stub's loaded image). NULL
// outside stub boot (then LoadExec is reported UNIMPL, as before).
void (*g_loadexec_hook)(Core*) = 0;

// Dispatch one A0/B0/C0 BIOS call. Returns 1 if handled (c->r[V0] set), 0 otherwise.
static int recomp_hle(char table, uint32_t fn, Core* c) {
  uint32_t a0 = c->r[A0], a1 = c->r[A1], a2 = c->r[A2];
  HleEvCB* s_ev = c->game->hle.ev;   // instance event table (alias keeps the EvCB code below unchanged)
  if (table == 'A') {
    switch (fn) {
      case 0x33: c->r[V0] = heap_alloc(c, a0); return 1;                 // malloc
      case 0x34: heap_free(c, a0); c->r[V0] = 0; return 1;               // free
      case 0x37: { uint32_t n = a0 * a1, p = heap_alloc(c, n);           // calloc
                   if (p) for (uint32_t i = 0; i < n; i++) c->mem_w8(p + i, 0);
                   c->r[V0] = p; return 1; }
      case 0x38: { uint32_t old = a0, ns = a1;                        // realloc
                   if (!old) { c->r[V0] = heap_alloc(c, ns); return 1; }
                   if (!ns) { heap_free(c, old); c->r[V0] = 0; return 1; }
                   uint32_t np = heap_alloc(c, ns), os = heap_block_size(c, old);
                   if (np) { uint32_t n = os < ns ? os : ns;
                             for (uint32_t i = 0; i < n; i++) c->mem_w8(np + i, c->mem_r8(old + i));
                             heap_free(c, old); }
                   c->r[V0] = np; return 1; }
      case 0x39: heap_init(c, a0, a1); c->r[V0] = 0; return 1;           // InitHeap
      case 0x44: c->r[V0] = 0; return 1;                              // FlushCache (no-op)
      case 0x49: c->r[V0] = 0; return 1;                              // GPU_cw(gp0): GP0
        // command word — drops to the (not-yet-wired) GPU; harmless until S5 renderer.
      case 0x51: if (g_loadexec_hook) { g_loadexec_hook(c); return 1; } return 0;  // LoadExec
      case 0x70: c->r[V0] = 0; return 1;                              // _bu_init (card) no-op
      case 0x71: c->r[V0] = 0; return 1;                              // _96_init (CD device) no-op
      case 0x72: c->r[V0] = 0; return 1;                              // _96_remove (no-op)
      default: {
        int card_hle_a0(uint32_t, Core*);     // native libcard A0 (_card_info/_card_load)
        if (card_hle_a0(fn, c)) return 1;
        return 0;
      }
    }
  }
  if (table == 'B') {
    switch (fn) {
      case 0x07:                                                      // DeliverEvent(cls,spec)
        hle_deliver_event(c, a0, a1); c->r[V0] = 0; return 1;
      case 0x08: {                                                    // OpenEvent(cls,spec,mode,func)
        for (int i = 0; i < EVCB_MAX; i++)
          if (!s_ev[i].open) {
            s_ev[i].open = 1; s_ev[i].enabled = 0; s_ev[i].fired = 0;
            s_ev[i].ev_class = a0; s_ev[i].spec = a1; s_ev[i].mode = a2; s_ev[i].func = c->r[A3];
            c->r[V0] = EV_ID_BASE + (uint32_t)i; return 1;
          }
        c->r[V0] = 0xFFFFFFFFu; return 1;                             // table full
      }
      case 0x09: { int i = ev_index(c, a0);                              // CloseEvent
        if (i >= 0) { s_ev[i].open = 0; c->r[V0] = 1; } else c->r[V0] = 0; return 1; }
      case 0x0A: { int i = ev_index(c, a0);                              // WaitEvent (can't block)
        if (i >= 0) { s_ev[i].fired = 0; c->r[V0] = 1; } else c->r[V0] = 0; return 1; }
      case 0x0B: { int i = ev_index(c, a0);                              // TestEvent (read+clear)
        if (i >= 0 && s_ev[i].fired) { s_ev[i].fired = 0; c->r[V0] = 1; } else c->r[V0] = 0;
        return 1; }
      case 0x0C: { int i = ev_index(c, a0);                              // EnableEvent
        if (i >= 0) { s_ev[i].enabled = 1; c->r[V0] = 1; } else c->r[V0] = 0; return 1; }
      case 0x0D: { int i = ev_index(c, a0);                              // DisableEvent
        if (i >= 0) { s_ev[i].enabled = 0; c->r[V0] = 1; } else c->r[V0] = 0; return 1; }
      case 0x12: case 0x13: case 0x14: case 0x15: case 0x16:          // BIOS pad — no-op
        c->r[V0] = 0; return 1;                                       // (pad serviced natively)
      case 0x19: c->game->hle.int_handler = a0; c->r[V0] = 0; return 1;          // HookEntryInt
      case 0x35: {                                                    // FileWrite
        uint32_t fd = a0, buf = a1, len = a2;
        if (fd == 1 || fd == 2) for (uint32_t i = 0; i < len; i++) fputc(c->mem_r8(buf + i), stderr);
        c->r[V0] = len; return 1;
      }
      case 0x0E: c->r[V0] = thread_open(c); return 1;                 // OpenThread
      case 0x0F: c->r[V0] = thread_close(c); return 1;                // CloseThread
      case 0x10: thread_change(c, a0); c->r[V0] = a0; return 1;       // ChangeThread
      case 0x4A: c->r[V0] = 0; return 1;                              // stopCard / card no-op
      case 0x4B: c->r[V0] = 1; return 1;                              // cardInfo -> present
      case 0x56: work_area_init(c); c->r[V0] = HLE_C0TABLE; return 1;  // GetC0Table
      case 0x57: work_area_init(c); c->r[V0] = HLE_B0TABLE; return 1;  // GetB0Table
      case 0x5B: c->r[V0] = 0; return 1;                              // ChangeClearPAD (no-op)
      default: {
        int card_hle_b0(uint32_t, Core*);     // native libcard (_card_read/write/status/info/chan)
        if (card_hle_b0(fn, c)) return 1;
        return 0;
      }
    }
  }
  if (table == 'C') {
    switch (fn) {
      case 0x02: case 0x03: c->r[V0] = a1; return 1;                  // SysEnqIntRP/DeqIntRP -> elem
      case 0x00: case 0x01: case 0x07: case 0x08:                     // kernel-table
      case 0x0A: case 0x0C: case 0x12: case 0x1C:                     // installers + RCnt
        c->r[V0] = 0; return 1;
      default: return 0;
    }
  }
  return 0;
}

// The `syscall` instruction: the kernel op is selected by $a0 (not the code field).
// Boot uses Enter/ExitCriticalSection around setup. Thread ops (a0=3) need the recomp
// thread model (not yet) — logged.
void rec_syscall(Core* c, uint32_t code) {
  (void)code;
  int& irq_enabled = c->game->hle.irq_enabled;   // was s_irq_enabled (now per-instance)
  switch (c->r[A0]) {
    case 0: c->r[V0] = 0; break;
    case 1: c->r[V0] = irq_enabled ? 1 : 0; irq_enabled = 0; break;  // EnterCritical
    case 2: irq_enabled = 1; c->r[V0] = 0; break;                    // ExitCritical
    default:
      fprintf(stderr, "[syscall] a0=%u (unhandled kernel op)\n", c->r[A0]);
      c->r[V0] = 0;
  }
}
void rec_break(Core* c, uint32_t code) {
  fprintf(stderr, "[break] code %u\n", code);
  (void)c;
}

void rec_interp(Core* c, uint32_t pc);  // hybrid fallback (interp.c)
OverrideFn rec_interp_override_for(uint32_t a);  // native override for an interpreted addr (interp.c)

static int g_miss = 0;
void rec_dispatch_miss(Core* c, uint32_t addr) {
  uint32_t a = addr & 0x1FFFFFFF;
  char tbl = a == 0xA0 ? 'A' : a == 0xB0 ? 'B' : a == 0xC0 ? 'C' : 0;
  if (tbl) {
    uint32_t fn = c->r[T1] & 0xFF;
    if (recomp_hle(tbl, fn, c)) return;
    fprintf(stderr, "[hle] UNIMPL %c0:0x%02X\n", tbl, fn);
    return;
  }
  // Non-recompiled code in RAM (loaded overlay, the boot stub, or an in-function computed-jump
  // target the recompiler routed here): run it with the hybrid interpreter. Skip the low
  // exception/scratchpad region (< 0x10000) which is never a call target. First honor a native
  // interp override for this address (the boot stub's libcd waits are replaced this way) — a
  // recompiled jalr into a non-recompiled stub fn enters here, bypassing call_addr's check.
  if (a >= 0x10000 && a < 0x200000) {
    // OVERRIDE SYSTEM REMOVED (2026-06-22): no native-override flip here either — interpret the RAM body.
    rec_interp(c, addr); return;
  }
  fprintf(stderr, "[miss %d] addr 0x%08X (no recompiled fn / overlay)\n", g_miss++, addr);
}

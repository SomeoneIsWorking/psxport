// Recomp-native HLE BIOS (S2 boot subset). Transcribed from the proven wide60 HLE
// (runtime/hle_kernel.cpp), adapted to operate directly on the R3000 register file and
// g_ram via the recomp memory accessors. Scope: exactly the A0/B0/C0 calls the boot path
// exercises; extended as the boot/diff harness reveals more. Faithful-first — semantics
// match the wide60 HLE that provably boots Tomba!2; not reimplemented from guesswork.
#include "r3000.h"
#include <stdio.h>
#include <string.h>

// MIPS o32 register indices (== c->r[]).
enum { A0=4, A1=5, A2=6, A3=7, T1=9, V0=2 };

// ---- Event control blocks (B0:0x07-0x0D) --------------------------------------------
// Transcribed from the proven wide60 HLE (hle_kernel.cpp). SCOPE: no scheduler/IRQ
// delivery in the recomp model, so WaitEvent cannot truly block — it reports the event
// ready and clears `fired`, which terminates the game's wait-then-test loops. A native
// frame source (timing.c) DeliverEvents the VBlank class each VSync so test loops advance.
typedef struct { int open, enabled, fired; uint32_t ev_class, spec, mode, func; } EvCB;
enum { EVCB_MAX = 16 };
static EvCB s_ev[EVCB_MAX];
static const uint32_t EV_ID_BASE = 0xF1000000u;
static int ev_index(uint32_t id) {
  uint32_t idx = id - EV_ID_BASE;
  return (idx < EVCB_MAX && s_ev[idx].open) ? (int)idx : -1;
}
// Native VBlank delivery (called by the frame tick): mark matching open+enabled slots fired.
void hle_deliver_event(uint32_t ev_class, uint32_t spec) {
  for (int i = 0; i < EVCB_MAX; i++)
    if (s_ev[i].open && s_ev[i].enabled && s_ev[i].ev_class == ev_class && (s_ev[i].spec & spec))
      s_ev[i].fired = 1;
}

// ---- Heap (A0:0x33-0x39): native first-fit arena, bookkeeping outside PSX RAM -------
typedef struct { uint32_t addr, size; int used; } HeapBlock;
enum { HEAP_MAX_BLOCKS = 4096 };
static HeapBlock s_blk[HEAP_MAX_BLOCKS];
static int s_nblk = 0;
static uint32_t s_heap_base = 0, s_heap_size = 0;
static int s_heap_ok = 0;

static void heap_init(uint32_t addr, uint32_t size) {
  s_heap_base = addr; s_heap_size = size;
  s_nblk = 1; s_blk[0].addr = addr; s_blk[0].size = size; s_blk[0].used = 0;
  s_heap_ok = 1;
}
static uint32_t heap_alloc(uint32_t size) {
  if (!s_heap_ok || size == 0) return 0;
  size = (size + 7u) & ~7u;
  for (int i = 0; i < s_nblk; i++) {
    if (s_blk[i].used || s_blk[i].size < size) continue;
    if (s_blk[i].size > size && s_nblk < HEAP_MAX_BLOCKS) {
      for (int j = s_nblk; j > i + 1; j--) s_blk[j] = s_blk[j - 1];
      s_blk[i + 1].addr = s_blk[i].addr + size;
      s_blk[i + 1].size = s_blk[i].size - size;
      s_blk[i + 1].used = 0;
      s_blk[i].size = size; s_nblk++;
    }
    s_blk[i].used = 1; return s_blk[i].addr;
  }
  return 0;
}
static void heap_coalesce(void) {
  for (int i = 0; i + 1 < s_nblk;) {
    if (!s_blk[i].used && !s_blk[i + 1].used) {
      s_blk[i].size += s_blk[i + 1].size;
      for (int j = i + 1; j + 1 < s_nblk; j++) s_blk[j] = s_blk[j + 1];
      s_nblk--;
    } else i++;
  }
}
static void heap_free(uint32_t addr) {
  if (!addr) return;
  for (int i = 0; i < s_nblk; i++)
    if (s_blk[i].addr == addr && s_blk[i].used) { s_blk[i].used = 0; heap_coalesce(); return; }
}
static uint32_t heap_block_size(uint32_t addr) {
  for (int i = 0; i < s_nblk; i++)
    if (s_blk[i].addr == addr && s_blk[i].used) return s_blk[i].size;
  return 0;
}

// ---- Native work area for GetB0Table()/GetC0Table() ---------------------------------
// Tomba2 reads B0table[+0x16C] -> control struct, deriving pointers it later uses.
// Publish a self-consistent native page so those reads land on valid memory.
enum { HLE_B0TABLE = 0x8000F000u, HLE_C0TABLE = 0x8000F800u, HLE_WORK_BASE = 0x8000E000u };
static int s_work_ok = 0;
static void work_area_init(void) {
  if (s_work_ok) return;
  s_work_ok = 1;
  mem_w32(HLE_B0TABLE + 0x16Cu, HLE_WORK_BASE);
  mem_w32(HLE_C0TABLE + 0, 0x03E00008u);  // jr $ra
  mem_w32(HLE_C0TABLE + 4, 0);            // nop
}

static uint32_t s_int_handler = 0;  // B0:0x19 HookEntryInt — game IRQ entry (recorded)

// ---- BIOS threads (B0:0x0E/0x0F/0x10) -----------------------------------------------
// LIMITATION: the static-recomp core uses the native C call stack, so a true BIOS
// ChangeThread (swap PC+regs to resume another thread) cannot be done by switching a
// register struct the way the wide60 interpreter does. OpenThread hands back a handle
// and records the entry PC; ChangeThread currently does NOT context-switch (stays on the
// current native stack). This is enough only while boot is straight-line; once the game
// actually relies on ChangeThread/coroutines (StrPlayer FMV prebuffer, the 0x80080860
// green-thread primitives) this needs a real coroutine override (ucontext/separate stack).
// STOPGAP: ChangeThread = no-op until the boot path is observed to need a real switch,
// because faking a switch wrongly is worse than not switching; revisit when boot reaches it.
enum { THREAD_MAX = 8 };
static uint32_t s_thread_pc[THREAD_MAX];
static int s_nthreads = 0;
static uint32_t thread_open(R3000* c) {
  if (s_nthreads >= THREAD_MAX) return 0xFFFFFFFFu;
  int i = s_nthreads++;
  s_thread_pc[i] = c->r[A1];  // a1 = entry PC (a2=sp, a3=gp) — recorded, not yet resumed
  return 0xFF000000u + (uint32_t)i;
}
static void thread_change(R3000* c, uint32_t handle) {
  (void)c; (void)handle;  // no real context switch yet (see LIMITATION above)
}

// Dispatch one A0/B0/C0 BIOS call. Returns 1 if handled (c->r[V0] set), 0 otherwise.
static int recomp_hle(char table, uint32_t fn, R3000* c) {
  uint32_t a0 = c->r[A0], a1 = c->r[A1], a2 = c->r[A2];
  if (table == 'A') {
    switch (fn) {
      case 0x33: c->r[V0] = heap_alloc(a0); return 1;                 // malloc
      case 0x34: heap_free(a0); c->r[V0] = 0; return 1;               // free
      case 0x37: { uint32_t n = a0 * a1, p = heap_alloc(n);           // calloc
                   if (p) for (uint32_t i = 0; i < n; i++) mem_w8(p + i, 0);
                   c->r[V0] = p; return 1; }
      case 0x38: { uint32_t old = a0, ns = a1;                        // realloc
                   if (!old) { c->r[V0] = heap_alloc(ns); return 1; }
                   if (!ns) { heap_free(old); c->r[V0] = 0; return 1; }
                   uint32_t np = heap_alloc(ns), os = heap_block_size(old);
                   if (np) { uint32_t n = os < ns ? os : ns;
                             for (uint32_t i = 0; i < n; i++) mem_w8(np + i, mem_r8(old + i));
                             heap_free(old); }
                   c->r[V0] = np; return 1; }
      case 0x39: heap_init(a0, a1); c->r[V0] = 0; return 1;           // InitHeap
      case 0x44: c->r[V0] = 0; return 1;                              // FlushCache (no-op)
      case 0x49: c->r[V0] = 0; return 1;                              // GPU_cw(gp0): GP0
        // command word — drops to the (not-yet-wired) GPU; harmless until S5 renderer.
      case 0x70: c->r[V0] = 0; return 1;                              // _bu_init (card) no-op
      case 0x72: c->r[V0] = 0; return 1;                              // _96_remove (no-op)
      default: return 0;
    }
  }
  if (table == 'B') {
    switch (fn) {
      case 0x07:                                                      // DeliverEvent(cls,spec)
        hle_deliver_event(a0, a1); c->r[V0] = 0; return 1;
      case 0x08: {                                                    // OpenEvent(cls,spec,mode,func)
        for (int i = 0; i < EVCB_MAX; i++)
          if (!s_ev[i].open) {
            s_ev[i].open = 1; s_ev[i].enabled = 0; s_ev[i].fired = 0;
            s_ev[i].ev_class = a0; s_ev[i].spec = a1; s_ev[i].mode = a2; s_ev[i].func = c->r[A3];
            c->r[V0] = EV_ID_BASE + (uint32_t)i; return 1;
          }
        c->r[V0] = 0xFFFFFFFFu; return 1;                             // table full
      }
      case 0x09: { int i = ev_index(a0);                              // CloseEvent
        if (i >= 0) { s_ev[i].open = 0; c->r[V0] = 1; } else c->r[V0] = 0; return 1; }
      case 0x0A: { int i = ev_index(a0);                              // WaitEvent (can't block)
        if (i >= 0) { s_ev[i].fired = 0; c->r[V0] = 1; } else c->r[V0] = 0; return 1; }
      case 0x0B: { int i = ev_index(a0);                              // TestEvent (read+clear)
        if (i >= 0 && s_ev[i].fired) { s_ev[i].fired = 0; c->r[V0] = 1; } else c->r[V0] = 0;
        return 1; }
      case 0x0C: { int i = ev_index(a0);                              // EnableEvent
        if (i >= 0) { s_ev[i].enabled = 1; c->r[V0] = 1; } else c->r[V0] = 0; return 1; }
      case 0x0D: { int i = ev_index(a0);                              // DisableEvent
        if (i >= 0) { s_ev[i].enabled = 0; c->r[V0] = 1; } else c->r[V0] = 0; return 1; }
      case 0x12: case 0x13: case 0x14: case 0x15: case 0x16:          // BIOS pad — no-op
        c->r[V0] = 0; return 1;                                       // (pad serviced natively)
      case 0x19: s_int_handler = a0; c->r[V0] = 0; return 1;          // HookEntryInt
      case 0x35: {                                                    // FileWrite
        uint32_t fd = a0, buf = a1, len = a2;
        if (fd == 1 || fd == 2) for (uint32_t i = 0; i < len; i++) fputc(mem_r8(buf + i), stderr);
        c->r[V0] = len; return 1;
      }
      case 0x0E: c->r[V0] = thread_open(c); return 1;                 // OpenThread
      case 0x0F: c->r[V0] = 1; return 1;                              // CloseThread (no-op)
      case 0x10: thread_change(c, a0); c->r[V0] = a0; return 1;       // ChangeThread
      case 0x4A: c->r[V0] = 0; return 1;                              // stopCard / card no-op
      case 0x4B: c->r[V0] = 1; return 1;                              // cardInfo -> present
      case 0x56: work_area_init(); c->r[V0] = HLE_C0TABLE; return 1;  // GetC0Table
      case 0x57: work_area_init(); c->r[V0] = HLE_B0TABLE; return 1;  // GetB0Table
      case 0x5B: c->r[V0] = 0; return 1;                              // ChangeClearPAD (no-op)
      default: return 0;
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
static int s_irq_enabled = 1;
void rec_syscall(R3000* c, uint32_t code) {
  (void)code;
  switch (c->r[A0]) {
    case 0: c->r[V0] = 0; break;
    case 1: c->r[V0] = s_irq_enabled ? 1 : 0; s_irq_enabled = 0; break;  // EnterCritical
    case 2: s_irq_enabled = 1; c->r[V0] = 0; break;                      // ExitCritical
    default:
      fprintf(stderr, "[syscall] a0=%u (unhandled kernel op)\n", c->r[A0]);
      c->r[V0] = 0;
  }
}
void rec_break(R3000* c, uint32_t code) {
  fprintf(stderr, "[break] code %u\n", code);
  (void)c;
}

static int g_miss = 0;
void rec_dispatch_miss(R3000* c, uint32_t addr) {
  uint32_t a = addr & 0x1FFFFFFF;
  char tbl = a == 0xA0 ? 'A' : a == 0xB0 ? 'B' : a == 0xC0 ? 'C' : 0;
  if (tbl) {
    uint32_t fn = c->r[T1] & 0xFF;
    if (recomp_hle(tbl, fn, c)) return;
    fprintf(stderr, "[hle] UNIMPL %c0:0x%02X\n", tbl, fn);
    return;
  }
  fprintf(stderr, "[miss %d] addr 0x%08X (no recompiled fn / overlay)\n", g_miss++, addr);
}

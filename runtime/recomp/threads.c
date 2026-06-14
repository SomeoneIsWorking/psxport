// Native BIOS threads for the static-recompiled core (the cooperative-scheduler subsystem).
//
// Tomba2's task scheduler (FUN_80051e60) runs tasks as BIOS threads: OpenThread(pc,sp,gp)
// then ChangeThread(handle); a task yields by ChangeThread(0xFF000000) back to the main
// thread (handle 0xFF000000 = the BIOS default thread the scheduler runs on). In the recomp
// the CPU register file is a single shared R3000 passed as `c`, and execution lives on the
// native C call stack — so a real PC+reg context switch can't be done by swapping a struct.
//
// We give each PSX thread its own NATIVE stack (ucontext) and, on ChangeThread, (1) save the
// running thread's R3000 regs and (2) restore the target's, then swapcontext to the target's
// native stack. A freshly OpenThread'd thread starts in a trampoline that enters the recomp
// body for its entry PC; when that returns (thread finished) it switches back to the main
// thread. This is the classic static-recomp coroutine solution (recomp-overrides skill).
#include "r3000.h"
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>

void rec_dispatch(R3000* c, uint32_t addr);

#define THR_MAX     16
#define THR_STACK   (1u << 20)   // 1 MB native C stack per thread
#define THR_BASE    0xFF000000u  // BIOS thread-handle base; index 0 = main/scheduler thread

typedef struct {
  int       used;        // slot allocated
  int       started;     // native context entered at least once
  uint32_t  regs[34];    // saved R3000: r[0..31], hi, lo
  uint32_t  entry_pc;    // recomp entry for a fresh thread
  ucontext_t uctx;       // native execution context
  void*     stack;       // native stack (NULL for main, which uses the real C stack)
} Thread;

static Thread  s_thr[THR_MAX];
static int     s_cur = 0;        // running thread index (0 = main)
static R3000*  s_cpu = 0;        // the single shared R3000 (all threads operate on it)
static int     s_init = 0;

static int handle_index(uint32_t h) {
  uint32_t i = h - THR_BASE;
  return (i < THR_MAX && s_thr[i].used) ? (int)i : -1;
}
static void save_regs(int i, R3000* c) {
  for (int k = 0; k < 32; k++) s_thr[i].regs[k] = c->r[k];
  s_thr[i].regs[32] = c->hi; s_thr[i].regs[33] = c->lo;
}
static void load_regs(int i, R3000* c) {
  for (int k = 0; k < 32; k++) c->r[k] = s_thr[i].regs[k];
  c->hi = s_thr[i].regs[32]; c->lo = s_thr[i].regs[33];
}

void threads_init(R3000* c) {
  if (s_init) return;
  s_init = 1; s_cpu = c;
  s_thr[0].used = 1; s_thr[0].started = 1; s_thr[0].stack = 0;  // main thread = slot 0
  s_cur = 0;
}

// makecontext trampoline: run the new thread's recomp body, then return to the main thread.
static void thread_entry(void) {
  int i = s_cur;                       // s_cur was set to this thread by thread_change
  rec_dispatch(s_cpu, s_thr[i].entry_pc);   // c->r already holds this thread's regs
  s_thr[i].used = 0;                   // thread finished -> slot free (stack freed on reuse,
  s_cur = 0; load_regs(0, s_cpu);      // never here: we're still running on this stack)
  setcontext(&s_thr[0].uctx);          // resume main where it last switched away
}

// B0:0x0E OpenThread(pc, sp, gp) -> handle (0xFF0000NN), -1 on full.
uint32_t thread_open(R3000* c) {
  int i = 1;
  for (; i < THR_MAX; i++) if (!s_thr[i].used) break;
  if (i >= THR_MAX) return 0xFFFFFFFFu;
  Thread* t = &s_thr[i];
  if (t->stack && i != s_cur) { free(t->stack); t->stack = 0; }  // reclaim a closed thread's
  t->used = 1; t->started = 0; t->entry_pc = c->r[4];   // a0=pc  // stack here, never live
  for (int k = 0; k < 34; k++) t->regs[k] = 0;
  t->regs[29] = c->r[5];   // sp = a1
  t->regs[28] = c->r[6];   // gp = a2
  t->stack = malloc(THR_STACK);
  getcontext(&t->uctx);
  t->uctx.uc_stack.ss_sp = t->stack;
  t->uctx.uc_stack.ss_size = THR_STACK;
  t->uctx.uc_link = 0;     // thread_entry never returns (it setcontext's back to main)
  makecontext(&t->uctx, thread_entry, 0);
  return THR_BASE + (uint32_t)i;
}

// B0:0x0F CloseThread(handle) -> 1. A thread routinely closes ITSELF then ChangeThreads
// away, so we must NOT free its native stack here (we're still executing on it -> munmap
// crash). Just free the slot; the stack is reclaimed when the slot is reused (thread_open).
uint32_t thread_close(R3000* c) {
  int i = handle_index(c->r[4]);
  if (i > 0) s_thr[i].used = 0;
  return 1;
}

// B0:0x10 ChangeThread(handle): switch register file + native stack to the target thread.
void thread_change(R3000* c, uint32_t handle) {
  int tgt = handle_index(handle);
  if (tgt < 0 || tgt == s_cur) return;        // unknown/self -> no switch
  int cur = s_cur;
  save_regs(cur, c);
  s_cur = tgt;
  s_thr[tgt].started = 1;
  load_regs(tgt, c);
  swapcontext(&s_thr[cur].uctx, &s_thr[tgt].uctx);
  // Resumed later: whoever switched back already restored OUR regs into c. Continue.
}

// libapi gate-stub overrides (the game calls these, not the B0 vector directly).
static void ov_open_thread(R3000* c)   { c->r[2] = thread_open(c); }
static void ov_close_thread(R3000* c)  { c->r[2] = thread_close(c); }
static void ov_change_thread(R3000* c) { uint32_t h = c->r[4]; thread_change(c, h); c->r[2] = h; }

void threads_register_overrides(void) {
  rec_set_override(0x80080860u, ov_open_thread);    // FUN_80080860 OpenThread
  rec_set_override(0x80080870u, ov_close_thread);   // FUN_80080870 CloseThread
  rec_set_override(0x80080880u, ov_change_thread);  // FUN_80080880 ChangeThread
}

// BIOS "thread" stubs — ucontext-free.
//
// Tomba2's task scheduler (FUN_80051e60) runs tasks as BIOS threads (OpenThread/ChangeThread)
// and yields cooperatively. An earlier version of this file implemented that with ucontext
// coroutines (each PSX thread on its own native stack). That approach is removed: the PC port
// does NOT run the game's cooperative scheduler — the boot/intro is driven natively
// (see boot.c / native_fmv.c), so no green-thread context switching is needed or wanted.
//
// These stubs remain only so the libapi gate-stub overrides still resolve and the build links;
// they perform no stack switching and no ucontext. If/when the game-proper runtime is brought
// up, its execution model will be designed without ucontext rather than reintroduced here.
#include "r3000.h"

#define THR_BASE 0xFF000000u

void threads_init(R3000* c) { (void)c; }

// B0:0x0E OpenThread(pc,sp,gp) -> handle. No native stack is created (no coroutines).
uint32_t thread_open(R3000* c)  { (void)c; return THR_BASE; }
// B0:0x0F CloseThread(handle) -> 1.
uint32_t thread_close(R3000* c) { (void)c; return 1; }
// B0:0x10 ChangeThread(handle): no-op (no context switch in the native boot).
void     thread_change(R3000* c, uint32_t handle) { (void)c; (void)handle; }

static void ov_open_thread(R3000* c)   { c->r[2] = thread_open(c); }
static void ov_close_thread(R3000* c)  { c->r[2] = thread_close(c); }
static void ov_change_thread(R3000* c) { uint32_t h = c->r[4]; thread_change(c, h); c->r[2] = h; }

void threads_register_overrides(void) {
  rec_set_override(0x80080860u, ov_open_thread);    // FUN_80080860 OpenThread
  rec_set_override(0x80080870u, ov_close_thread);   // FUN_80080870 CloseThread
  rec_set_override(0x80080880u, ov_change_thread);  // FUN_80080880 ChangeThread
}

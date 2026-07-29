// Host turn — giving the HOST a turn while the guest executes straight-line recompiled code.
//
// THE PROBLEM THIS EXISTS FOR. A static recompile runs guest code as ordinary C functions. The host
// only regains control when the guest calls something the runtime owns. That covers most of a port,
// because a PSX game reaches libgpu/libcd/libetc constantly — but it does NOT cover a busy-wait that
// is paced by a per-vblank callback. Such a loop calls only game code, and its exit condition is a
// counter that only the callback advances. Nothing between two guest instructions advances time, so
// the loop cannot terminate. Spider-Man's boot has exactly this shape: a ~5 second title wait whose
// body is pad polling and whose bound is a counter written by the callback registered with
// VSyncCallback (spider1 docs/re-frontier.md RE-05).
//
// WHY THIS IS NOT INTERRUPT EMULATION. The port owns the frame clock: real elapsed time at the
// display field rate says how many fields have happened, and that is a hardware fact, not a tunable.
// This file does not model an interrupt controller, does not touch I_STAT/I_MASK, and does not run
// the BIOS interrupt-element chain. It says "the host is owed a turn" and lets the port take it —
// the same shape as the CD layer announcing a completion by invoking the callback the guest
// registered, which is the established pattern in this runtime.
//
// WHY THE PACING IS NOT A MAGIC NUMBER. The obvious alternative — give the host a turn every N
// recompiled calls — would need an N with no ground truth in the game or the hardware, so it could
// only be chosen by tuning until the symptom disappeared. It would also couple the guest's sense of
// TIME to its CALL DENSITY, so a stretch of code that calls fewer functions would experience slower
// time. There is no such constant here: the arming interval only affects how promptly a turn is
// taken, while the number of fields owed is always elapsed_time × field_rate.
#include "core.h"
#include "game.h"   // Game::hle.irq_enabled — the guest's critical-section flag
#include "cfg.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

// The registered handler and the Core it belongs to. A single Core is the norm; the divergence
// harness runs two, but only one of them is ever the live paced port, so one registration is right.
HostTurnFn        s_fn   = nullptr;
Core*             s_core = nullptr;
unsigned          s_fps_millihz = 0;

std::thread             s_thread;
std::mutex              s_m;
std::condition_variable s_cv;
bool                    s_stop = false;

// Re-entrancy guard. The handler runs guest code (it dispatches the game's registered callback), and
// that guest code enters recompiled functions, each of which tests the gate. Without this a turn
// could nest inside itself without bound.
bool s_in_turn = false;

// The timer. It does nothing but set the gate bit on the field clock; it never touches guest memory,
// so there is no shared mutable state beyond the one flag word. Running the WORK here instead would
// mean two threads in guest RAM at once, which is exactly what the gate exists to avoid: the turn is
// taken by the guest thread, at a call-coherent boundary, and this thread only says "you are owed
// one".
void timer_main() {
  const auto period = std::chrono::nanoseconds(1000000000000ull / s_fps_millihz);
  std::unique_lock<std::mutex> lk(s_m);
  while (!s_stop) {
    if (s_cv.wait_for(lk, period, [] { return s_stop; })) break;
    Core* c = s_core;
    // Relaxed is right: this is a hint word, and the guest thread re-derives the actual amount of
    // owed work from the clock. A missed or late set costs latency, never correctness.
    if (c) __atomic_or_fetch(&c->pending_work, Core::PW_HOST, __ATOMIC_RELAXED);
  }
}

}  // namespace

void rec_host_turn_register(Core* c, HostTurnFn fn, unsigned fps_millihz) {
  if (!c || !fn || !fps_millihz) {
    cfg_loge("hostturn", "refusing to register: core=%p fn=%p fps_millihz=%u — a zero field rate "
                         "would make the timer spin, and a null handler would arm a gate nothing "
                         "services.", (void*)c, (void*)fn, fps_millihz);
    return;
  }
  if (s_fn) {
    cfg_logw("hostturn", "already registered; ignoring the second registration");
    return;
  }
  s_fn = fn; s_core = c; s_fps_millihz = fps_millihz;
  s_thread = std::thread(timer_main);
  cfg_logi("hostturn", "host turn armed at %u.%03u Hz — the guest yields to the host at recompiled "
                       "function entry", fps_millihz / 1000, fps_millihz % 1000);
}

void rec_host_turn_shutdown() {
  if (!s_thread.joinable()) return;
  { std::lock_guard<std::mutex> lk(s_m); s_stop = true; }
  s_cv.notify_all();
  s_thread.join();
  s_fn = nullptr; s_core = nullptr;
}

// Called from rec_irq_poll (the substrate's gate entry) when PW_HOST is set.
void rec_host_turn(Core* c) {
  // RESPECT THE GUEST'S CRITICAL SECTIONS. The turn dispatches a callback the GUEST registered, so it
  // is guest code running at a moment the guest did not choose. When the guest has masked interrupts
  // (COP0 Status.IEc clear) it is saying exactly one thing: do not run my handlers here. Ignoring
  // that runs the callback in the middle of a non-atomic update and corrupts state.
  //
  // Hle::irqPoll has always made this check (`if (in_irq || !irq_enabled) return;`). The host turn
  // dispatches guest code for the same reason and must make it too.
  //
  // HONESTY NOTE: this check was added while chasing what looked like state corruption caused by the
  // loop-back-edge gate, and it did NOT fix that. It is kept because it is correct on its own merits.
  // The corruption turned out not to be the gate's at all — it was a branch-and-link mistranslation
  // in the recompiler leaking a stack frame (emit_control's BRANCH-AND-LINK note). Hypotheses
  // falsified on the way, none worth re-trying: register save/restore across the poll
  // (PSXPORT_DEBUG=pollregs reports zero clobbers), and this critical-section check.
  //
  // Do NOT clear PW_HOST when deferring: hardware would leave the VBlank latched and deliver it when
  // the guest re-enables. Leaving the bit set reproduces that — the turn is taken at the first gate
  // after the critical section ends, rather than being silently dropped.
  if (!c->game->hle.irq_enabled) return;

  // Clear only once the turn is actually being taken. The timer may set it again while the handler
  // runs — that is correct and means another field elapsed during the turn.
  __atomic_and_fetch(&c->pending_work, ~Core::PW_HOST, __ATOMIC_RELAXED);

  if (s_in_turn || !s_fn || c != s_core) return;
  // The same transient-state check interrupt delivery makes (hle.cpp): if either is live we are in
  // the middle of the dispatch machinery, not at a clean boundary, and running guest code here could
  // lose a pending redirect.
  if (c->override_tgt || c->coro_redirect_pc) return;

  s_in_turn = true;
  // Full guest-context save/restore. The handler dispatches a guest callback, which will use the
  // register file; the function whose entry we intercepted has not run a single instruction yet and
  // must see its arguments intact.
  const R3000 saved = *static_cast<R3000*>(c);
  s_fn(c);
  *static_cast<R3000*>(c) = saved;
  s_in_turn = false;
}

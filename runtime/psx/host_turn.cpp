// The field clock requests host service without touching guest state from its timer thread. The
// guest thread performs the callback at an executor boundary after honoring critical sections.
#include "host_turn.h"
#include "core.h"
#include "game.h" // Game::hle.irq_enabled — the guest's critical-section flag
#include "host_turn_plan.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib> // std::atexit — the timer thread joins itself at exit (see below)
#include <lucent/log.h>
#include <mutex>
#include <thread>

namespace {

// The registered handler and the Core it belongs to. A single Core is the norm; the divergence
// harness runs two, but only one of them is ever the live paced port, so one registration is right.
psx::cpu::HostTurnFunction s_fn = nullptr;
Core *s_core = nullptr;
unsigned s_fps_millihz = 0;

std::thread s_thread;
std::mutex s_m;
std::condition_variable s_cv;
bool s_stop = false;
HostTurnClockState s_clock;

int64_t steady_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Re-entrancy guard. The handler runs guest code (it dispatches the game's registered callback), and
// that guest code enters guest functions, each of which tests the gate. Without this a turn
// could nest inside itself without bound.
bool s_in_turn = false;

// The timer. It does nothing but set the gate bit on the field clock; it never touches guest memory,
// so there is no shared mutable state beyond the one flag word. Running the WORK here instead would
// mean two threads in guest RAM at once, which is exactly what the gate exists to avoid: the turn is
// taken by the guest thread, at a call-coherent boundary, and this thread only says "you are owed
// one".
void timer_main() {
  const auto period = std::chrono::nanoseconds(1000000000000ull / s_fps_millihz);
  const int64_t period_ns = period.count();
  std::unique_lock<std::mutex> lk(s_m);
  while (!s_stop) {
    const uint64_t generation = s_clock.generation;
    const auto deadline = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(s_clock.deadline_ns));
    s_cv.wait_until(lk, deadline, [generation] {
      return s_stop || s_clock.generation != generation;
    });
    const int64_t now_ns = steady_now_ns();
    const HostTurnWakeAction action = host_turn_wake_action(generation, s_clock, now_ns, s_stop);
    if (action == HostTurnWakeAction::Stop) {
      break;
    }
    if (action == HostTurnWakeAction::Restart) {
      continue;
    }
    Core *c = s_core;
    // Relaxed is right: this is a hint word, and the guest thread re-derives the actual amount of
    // owed work from the clock. A missed or late set costs latency, never correctness.
    if (c) {
      __atomic_or_fetch(&c->pending_work, Core::PW_HOST, __ATOMIC_RELAXED);
    }
    s_clock = host_turn_clock_armed(s_clock, now_ns, period_ns);
  }
}

void host_turn_field_delivered(Core *c) {
  if (!c || c != s_core) {
    return;
  }
  // An explicit guest/native field and the timer represent the same hardware event. Cancel any
  // already-latched host turn and restart the timer from this completed field; otherwise a timer
  // tick that occurred while the explicit path was pacing is delivered immediately afterward and
  // the game runs at nearly twice its video standard.
  {
    std::lock_guard<std::mutex> lk(s_m);
    const int64_t period_ns = (int64_t)(1000000000000ull / s_fps_millihz);
    s_clock = host_turn_clock_field_delivered(s_clock, steady_now_ns(), period_ns);
    // Clear while holding the timer's mutex. If the old deadline is expiring concurrently, either
    // the timer arms first and this clears it, or this advances generation first and the timer's
    // stale wake restarts. Clearing before taking the lock leaves a third ordering where the timer
    // re-arms the obsolete field between the clear and generation change.
    __atomic_and_fetch(&c->pending_work, host_turn_pending_after_field(~0, Core::PW_HOST), __ATOMIC_RELAXED);
  }
  s_cv.notify_all();
}

} // namespace

void psx::cpu::notifyDisplayField(Core &core) {
  host_turn_field_delivered(&core);
}

void psx::cpu::registerHostTurn(Core &core, HostTurnFunction fn, unsigned fps_millihz) {
  if (!fn || !fps_millihz) {
    lucent::error("hostturn",
                  "refusing to register: core={} fn={} fps_millihz={} — a zero field rate "
                  "would make the timer spin, and a null handler would arm a gate nothing "
                  "services.",
                  (void *)&core,
                  (void *)fn,
                  fps_millihz);
    return;
  }
  if (s_fn) {
    lucent::warn("hostturn", "already registered; ignoring the second registration");
    return;
  }
  s_fn = fn;
  s_core = &core;
  s_fps_millihz = fps_millihz;
  {
    std::lock_guard<std::mutex> lk(s_m);
    s_stop = false;
    const int64_t period_ns = (int64_t)(1000000000000ull / s_fps_millihz);
    s_clock = host_turn_clock_start(steady_now_ns(), period_ns);
  }
  s_thread = std::thread(timer_main);
  // THE THREAD JOINS ITSELF AT EXIT, and it has to be armed HERE rather than left to the port.
  // rec_host_turn_shutdown() existed for this and had ZERO callers anywhere — framework, tests or any
  // port — for one reason: until 2026-08-12 no psxport port had ever reached a clean exit, so a still-
  // joinable std::thread at static destruction never got the chance to abort. The first port that DID
  // exit cleanly (spyro, once the producer-DB run cap gave it a last frame) died with rc=139 and
  // "terminate called without an active exception" in std::thread::~thread — a failure that looks like a
  // crash in the port and is actually the framework never shutting its own thread down.
  //
  // Registered once, next to the only place the thread is created, so no port can forget it and no port
  // has to know it exists. Idempotent: shutdown() early-returns when the thread is not joinable, so an
  // explicit call from a port (spyro's producer_run.cpp does one) costs nothing and the atexit path is
  // still there for the ports that do not.
  static bool atexit_armed = false;
  if (!atexit_armed) {
    atexit_armed = true;
    std::atexit(+[] {
      psx::cpu::shutdownHostTurn();
    });
  }
  lucent::info("hostturn", "host turn armed at {}.{:03} Hz", fps_millihz / 1000, fps_millihz % 1000);
}

void psx::cpu::shutdownHostTurn() {
  if (!s_thread.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lk(s_m);
    s_stop = true;
  }
  s_cv.notify_all();
  s_thread.join();
  s_fn = nullptr;
  s_core = nullptr;
}

void psx::cpu::serviceHostTurn(Core &core) {
  Core *c = &core;
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
  // in the executor leaking a stack frame (emit_control's BRANCH-AND-LINK note). Hypotheses
  // falsified on the way, none worth re-trying: register save/restore across the poll
  // (PSXPORT_DEBUG=pollregs reports zero clobbers), and this critical-section check.
  //
  // Do NOT clear PW_HOST when deferring: hardware would leave the VBlank latched and deliver it when
  // the guest re-enables. Leaving the bit set reproduces that — the turn is taken at the first gate
  // after the critical section ends, rather than being silently dropped.
  if (!c->game->hle.irq_enabled) {
    return;
  }

  // Clear only once the turn is actually being taken. The timer may set it again while the handler
  // runs — that is correct and means another field elapsed during the turn.
  __atomic_and_fetch(&c->pending_work, ~Core::PW_HOST, __ATOMIC_RELAXED);

  if (s_in_turn || !s_fn || c != s_core) {
    return;
  }
  // The same transient-state check interrupt delivery makes (hle.cpp): if either is live we are in
  // the middle of the dispatch machinery, not at a clean boundary, and running guest code here could
  // lose a pending redirect.
  if (c->active_native_address || c->pending_guest_redirect) {
    return;
  }

  s_in_turn = true;
  // Full guest-context save/restore. The handler dispatches a guest callback, which will use the
  // register file; the function whose entry we intercepted has not run a single instruction yet and
  // must see its arguments intact.
  const R3000 saved = *static_cast<R3000 *>(c);
  s_fn(c);
  *static_cast<R3000 *>(c) = saved;
  s_in_turn = false;
}

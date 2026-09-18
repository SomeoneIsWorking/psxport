// The host field clock. It owes the registered handler one display field per field period of
// EMULATED guest time, and the guest thread takes that turn at an executor boundary after honoring
// its critical sections. There is no timer thread: the previous wall-clock timer made the number of
// fields a long guest update observed depend on host speed (a cold Lightrec translation or an unpaced
// run delivered two extra fields inside Spyro 1's level-loader hand-off, so its first gameplay update
// integrated with g_DeltaTime 4 where the console reads 2), and it made every such hand-off vary
// between runs. Guest time is what the console's VBlank counts, so it is what owes fields here.
#include "host_turn.h"
#include "core.h"
#include "emulated_time.h"
#include "game.h"
#include <limits>
#include <lucent/log.h>

namespace {

// The registered handler and the Core it belongs to. A single Core is the norm; the divergence
// harness runs two, but only one of them is ever the live paced port, so one registration is right.
psx::cpu::HostTurnFunction s_fn = nullptr;
Core *s_core = nullptr;
uint64_t s_periodTicks = 0;
// Emulated CPU tick at which the next field is owed. A delivered field is a boundary and starts a
// complete period; a served turn does the same, so an update longer than several periods sees one
// field per period rather than a burst.
uint64_t s_deadlineTicks = 0;

// Re-entrancy guard. The handler runs guest code (it dispatches the game's registered callback), and
// that guest code enters guest functions, each of which tests the gate. Without this a turn
// could nest inside itself without bound.
bool s_in_turn = false;

uint64_t guestNow(const Core &core) {
  return core.game->timing.emulatedCpuTicks();
}

} // namespace

void psx::cpu::registerHostTurn(Core &core, HostTurnFunction fn, unsigned fps_millihz) {
  if (!fn || !fps_millihz) {
    lucent::error("hostturn",
                  "refusing to register: core={} fn={} fps_millihz={} — a zero field rate "
                  "would owe a field every instruction, and a null handler would arm a gate nothing "
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
  s_periodTicks = display_field_cpu_ticks(1, 1, fps_millihz);
  s_deadlineTicks = guestNow(core) + s_periodTicks;
  lucent::info("hostturn",
               "host turn armed at {}.{:03} Hz: one field per {} guest ticks",
               fps_millihz / 1000,
               fps_millihz % 1000,
               s_periodTicks);
}

void psx::cpu::shutdownHostTurn() {
  s_fn = nullptr;
  s_core = nullptr;
  s_periodTicks = 0;
  s_deadlineTicks = 0;
}

void psx::cpu::requestHostTurnWhenDue(Core &core) {
  if (&core != s_core || guestNow(core) < s_deadlineTicks) {
    return;
  }
  // A hint word: the guest thread re-derives the owed work from the clock at the boundary it takes
  // the turn, and a turn already pending stays pending.
  const int previous = __atomic_fetch_or(&core.pending_work, Core::PW_HOST, __ATOMIC_RELAXED);
  if ((previous & Core::PW_HOST) == 0) {
    lucent::debug("hostturn",
                  "field due: now={} deadline={} (overrun {} ticks)",
                  guestNow(core),
                  s_deadlineTicks,
                  guestNow(core) - s_deadlineTicks);
  }
}

std::uint64_t psx::cpu::hostTurnTicksUntilDue(const Core &core) {
  if (&core != s_core) {
    return std::numeric_limits<uint64_t>::max();
  }
  const uint64_t now = guestNow(core);
  return now < s_deadlineTicks ? s_deadlineTicks - now : 0;
}

void psx::cpu::notifyDisplayField(Core &core) {
  if (&core != s_core) {
    return;
  }
  // An explicit guest/native field and the clock represent the same hardware event. Cancel any
  // already-latched host turn and start the next complete period from this completed field;
  // otherwise a field owed while the explicit path was delivering is served immediately afterward
  // and the game runs at nearly twice its video standard.
  s_deadlineTicks = guestNow(core) + s_periodTicks;
  __atomic_and_fetch(&core.pending_work, ~Core::PW_HOST, __ATOMIC_RELAXED);
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
  // Do NOT clear PW_HOST when deferring: hardware would leave the VBlank latched and deliver it when
  // the guest re-enables. Leaving the bit set reproduces that — the turn is taken at the first gate
  // after the critical section ends, rather than being silently dropped.
  if (c->game->hle.in_irq || !c->game->hle.irq_enabled) {
    return;
  }

  const bool ownsHandler = s_fn && c == s_core;
  if (ownsHandler && s_in_turn) {
    lucent::debug("hostturn", "turn deferred: nested inside the handler at pc=0x{:08X}", c->pc);
    return;
  }
  // The same transient-state check interrupt delivery makes (hle.cpp): if either is live we are in
  // the middle of the dispatch machinery, not at a clean boundary, and running guest code here could
  // lose a pending redirect.
  if (c->active_native_address || c->pending_guest_redirect) {
    return;
  }

  // At an eligible boundary, consume this Core's request. A Core without a registered handler
  // retires a stale request here; it must never invoke another Core's callback. The clock may owe
  // a new field while the handler runs, and that request remains owed afterward.
  __atomic_and_fetch(&c->pending_work, ~Core::PW_HOST, __ATOMIC_RELAXED);
  if (!ownsHandler) {
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
  // A served turn is a field boundary whether or not the handler acknowledged it explicitly.
  if (s_deadlineTicks <= guestNow(*c)) {
    s_deadlineTicks = guestNow(*c) + s_periodTicks;
  }
  lucent::debug("hostturn",
                "turn served at pc=0x{:08X}: now={} next deadline={} pending=0x{:X}",
                c->pc,
                guestNow(*c),
                s_deadlineTicks,
                c->pending_work);
}

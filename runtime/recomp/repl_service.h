#ifndef PSXPORT_REPL_SERVICE_H
#define PSXPORT_REPL_SERVICE_H
// repl_service.h — "is anybody going to READ this stdin?", decided in one place.
//
// WHY THIS FILE EXISTS. `Repl::read()` (repl.cpp) is pumped by exactly ONE loop: the single-core
// native frame loop in native_boot.cpp. Every other loop that can own the process — the SBS
// two-core harness (sbs.cpp), DualCore (dualcore.cpp) and the selftests (selftest.cpp), all three
// dispatched from a game's own main() — never touches stdin, and ALL THREE now call the guard
// below. So a piped REPL script handed to one of those runs is not rejected and not executed; it
// sits in the pipe while the harness runs its own default script, and the run LOOKS like it worked.
// That is how a crash report on 2026-08-12 came to blame "SBS + a REPL-driven newgame" for a run
// that never left attract mode (Tomba2Engine kanban #90).
//
// The house rule is "silently-skipped input is a failure, not a filter", so the answer is a LOUD
// REFUSAL rather than a best-effort: the run exits NON-ZERO naming what it did not service and how
// to drive an SBS run for real (PSXPORT_SBS_AUTONAV / PSXPORT_SBS_WARP / PSXPORT_SBS_PAD_REPLAY /
// PSXPORT_DEBUG_SERVER).
//
// WHY NOT "just service the REPL from the SBS loop too" (option (b), considered and rejected):
// the REPL drives ONE Core. Its interesting commands are `press`/`hold`, `newgame`, `skip`, `warp`
// — pad and game-state drive. Applying any of them to the `sbs show`-selected core only would
// inject a divergence the byte-compare then reports as a port bug, i.e. it would turn the debugger
// into a divergence SOURCE. Applying them to both cores identically means re-implementing
// native_boot.cpp's armed-request consumers (the newgame prologue pulser, `skip`, the door-record
// warp) against two cores in lockstep — which is precisely what `Sbs::Impl::navStep` +
// PSXPORT_SBS_WARP already are, in a form that already refuses when the nav addresses are unset.
// A second, single-core copy of that mechanism next to it is a fork of the thing that works.
//
// THE POLICY IS PURE (this header) AND THE PROBE IS NOT (repl_service.cpp) so the decision can be
// gated hermetically on BOTH classes — a refusal that fires on every headless `< /dev/null` run
// would be worse than the bug, because operators learn to ignore it.
#include <string>

namespace psx {
namespace repl_service {

// Exit status of a refusal. 2, not 1: it matches the "a refusal is not a test failure" convention
// the game gate tools already use (Tomba2Engine/tools/gate.py exits 2 when it refuses to run).
static const int kRefusalExit = 2;

enum class Verdict {
  Ok,      // nothing is being dropped: either the loop pumps the REPL, or no input is waiting
  Warn,    // a human is typing at a terminal into a loop that will not read it — say so, keep going
  Refuse,  // a script's commands are being dropped on the floor — the run's conclusion is worthless
};

struct Query {
  // Does the loop that owns the process actually call Repl::read()? True only for the single-core
  // native frame loop. This field is what makes the decision testable on its NEGATIVE class.
  bool loopServicesRepl = false;
  // PSXPORT_REPL — the operator explicitly asked for an interactive REPL.
  bool replRequested = false;
  // Is fd 0 a terminal? A human at a prompt gets a warning, not a killed session.
  bool stdinIsTty = false;
  // Bytes ALREADY waiting on fd 0: >0 = a driver has written commands; 0 = nothing waiting (an
  // empty pipe, or `< /dev/null` at EOF); <0 = cannot tell (unknown fd type / ioctl failed).
  // "Cannot tell" must never produce a refusal — see decide().
  long stdinPendingBytes = -1;
};

inline Verdict decide(const Query& q) {
  // The loop reads stdin itself: nothing is being dropped, whatever is in the pipe.
  if (q.loopServicesRepl) return Verdict::Ok;
  // An explicit PSXPORT_REPL against a loop with no pump is unambiguous, pending bytes or not: the
  // operator asked for a REPL this run cannot give them. This is the case that needs no timing luck.
  if (q.replRequested) return Verdict::Refuse;
  // No explicit request: the evidence is bytes waiting on the pipe. This is the repro that started
  // it (`printf 'newgame\nrun 200\nquit\n' | PSXPORT_SBS_MODE=full …` sets no PSXPORT_REPL at all).
  if (q.stdinPendingBytes > 0) return q.stdinIsTty ? Verdict::Warn : Verdict::Refuse;
  // 0 bytes waiting (`< /dev/null`, an idle inherited pipe) or an fd we cannot measure: a run with
  // no input to drop is a normal headless run and must stay silent.
  return Verdict::Ok;
}

// ── per-loop drive advice ───────────────────────────────────────────────────────────────────────
// "Use this instead" is DIFFERENT for every harness — PSXPORT_SBS_AUTONAV means nothing to a
// DualCore run — so the advice is a registry keyed by the loop, not one paragraph with the loop's
// name pasted in. It returns nullptr for a name nobody registered, and message() then says so out
// loud: an unregistered loop must read as "this harness has no registered advice", never as
// silently-correct SBS advice. That is what makes the typo case (`"sbs"`, a new harness added
// without a paragraph) visible instead of a no-op — and tests/test_repl_unserviced_refusal.cpp
// scans every call site in the runtime to check the name it passes IS registered.
inline const char* drive_advice(const char* loopName) {
  const std::string n = loopName ? loopName : "";
  if (n == "SBS")
    return "To DRIVE an SBS run use the harness's own two-core mechanisms, which apply to BOTH "
           "cores identically: PSXPORT_SBS_AUTONAV=1 (auto-nav to the field), "
           "PSXPORT_SBS_WARP=frame:area[:sub] (deterministic area load), "
           "PSXPORT_SBS_PAD_REPLAY=<file.pad> (recorded input), or PSXPORT_DEBUG_SERVER=1 (live "
           "socket). For the REPL itself, run the SINGLE-CORE port without "
           "PSXPORT_SBS/PSXPORT_SBS_MODE.";
  if (n == "DualCore")
    return "A DualCore run (PSXPORT_DUALCORE=1) is a one-shot diagnostic that drives ITSELF: its "
           "nav machine taps Cross to the GAME stage and pulses Start through the cutscene, then "
           "it diffs guest RAM between the native-render and PSX-render legs. There is no operator "
           "input to give it; PSXPORT_DC_ALL=1 is the only knob that changes its report. For the "
           "REPL itself, run the SINGLE-CORE port without PSXPORT_DUALCORE.";
  if (n == "selftest")
    return "A PSXPORT_SELFTEST run is a FIXED script with a pass/fail exit code and no operator "
           "input, so there is nothing for a REPL command to drive; PSXPORT_SELFTEST_VERBOSE=1 is "
           "what changes what it prints. For the REPL itself, run the SINGLE-CORE port without "
           "PSXPORT_SELFTEST.";
  return nullptr;   // unregistered: message() states that rather than inventing advice
}

// The operator-facing text. Built here, next to the rule, so the test can assert what a refusal
// SAYS — a refusal that does not name the unserviced thing and the supported alternative just
// looks like a crash. `loopName` is the loop that owns the process ("SBS", "DualCore", …) and must
// be one of the names drive_advice() registers.
inline std::string message(const char* loopName, const Query& q, Verdict v) {
  const std::string loop = loopName ? loopName : "this harness";
  std::string s;
  if (v == Verdict::Ok) return s;   // callers must not print anything on Ok
  s += "the " + loop + " loop does NOT service the REPL — ";
  if (q.replRequested)
    s += "PSXPORT_REPL is set, but Repl::read() is pumped ONLY by the single-core native frame loop "
         "(runtime/recomp/native_boot.cpp), which this run never enters. ";
  if (q.stdinPendingBytes > 0)
    s += std::to_string(q.stdinPendingBytes) +
         " byte(s) are ALREADY WAITING on stdin and NOTHING in this run will ever read them: every "
         "command in that script would be silently discarded while the " + loop + " harness ran its "
         "own script, and the run would look like it had worked. ";
  if (const char* advice = drive_advice(loopName)) {
    s += advice;
    s += " ";
  } else {
    // Honest fallback. It must not read like advice, because it isn't: nobody registered this loop.
    s += "NO DRIVE MECHANISM IS REGISTERED for a loop called \"" + loop +
         "\" (repl_service.h drive_advice() knows: SBS, DualCore, selftest), so this refusal cannot "
         "tell you what to use instead — that is a gap in the guard, not in your command line. For "
         "the REPL itself, run the SINGLE-CORE port. ";
  }
  if (v == Verdict::Refuse)
    s += "REFUSING THE RUN (exit " + std::to_string(kRefusalExit) +
         ") rather than producing a verdict over input it threw away.";
  else
    s += "Nothing in this run is reading your terminal; drive it with the mechanisms above. "
         "(Continuing — a live session is not killed for a keystroke.)";
  return s;
}

// ── the impure half (repl_service.cpp) ──────────────────────────────────────────────────────────
// Bytes waiting on fd 0, or <0 for "cannot tell". Regular file: size-minus-offset. FIFO/socket/
// char device: FIONREAD. NOTE the trap this exists to avoid: poll(POLLIN) reports READABLE at EOF,
// so `< /dev/null` and a closed empty pipe would both look like a waiting script.
long stdin_pending_bytes();

// Probe the process + decide + report. On Verdict::Refuse this calls exit(kRefusalExit) and does
// not return; on Warn it logs at most once per process; on Ok it is silent. `loopServicesRepl` is
// the caller stating whether ITS loop pumps Repl::read().
void refuse_if_unserviced(const char* loopName, bool loopServicesRepl = false);

}  // namespace repl_service
}  // namespace psx

#endif  // PSXPORT_REPL_SERVICE_H

// test_repl_unserviced_refusal.cpp — the SBS harness must never SWALLOW a piped REPL script.
//
// THE DEFECT THIS PINS (Tomba2Engine kanban #90, and it already produced a false conclusion).
// `Game::repl.read()` has exactly ONE caller: the SINGLE-CORE frame loop in
// runtime/recomp/native_boot.cpp. `runtime/recomp/sbs.cpp` never reads stdin at all. So
//
//     printf 'newgame\nrun 200\nquit\n' | PSXPORT_SBS_MODE=full ./scratch/bin/<port> <exe>
//
// does not "accept every command and throw it away" in the sense of parsing them — it never even
// looks at the pipe. main() routes to Sbs::run() before native_boot is ever reached, the harness
// runs a plain no-autonav lockstep from boot, and the run LOOKS like it worked. On 2026-08-12 that
// made a crash report blame "SBS + a REPL-driven newgame" for a run that never left attract mode.
//
// This is the house rule "silently-skipped input is a failure, not a filter". So there are FOUR
// groups of assertions here (1 wiring, 2 policy, 2b the shipping function, 3 the knob), and the
// wiring one is the load-bearing one — it is the case that was RED before the fix:
//
//   1. WIRING (source-level): sbs.cpp must contain, textually, EITHER a call to the refusal guard
//      (option (a): refuse loudly) OR a `repl.read(` call site (option (b): actually service the
//      REPL from the SBS loop). One of the two must be true, forever. A source scan is used
//      deliberately: the alternative — booting the SBS harness — needs a disc, a substrate and a
//      GPU, so it cannot be a hermetic gate, and a policy function nobody CALLS is exactly the
//      "same bug one level up" this file exists to prevent.
//   2. POLICY (pure): the decision function must REFUSE the piped case and stay quiet on the cases
//      that are legitimately non-interactive (`< /dev/null`, an idle inherited pipe), and it is
//      exercised on BOTH classes — a discriminator only ever run on one class has not been tested.
//   2b. THE SHIPPING FUNCTION FIRES: the real refuse_if_unserviced(), probe and exit() included, run
//      in a forked child against a controlled fd 0, asserted on the child's exit code.
//   3. THE KNOB: PSXPORT_REPL resolves through the CVar ladder identically to its pre-migration body.
//
// WHAT A NEGATIVE PRINTS HERE: every scan states its denominator (files read, bytes read, call
// sites found per file) and REFUSES (fails) when a file it must read is missing, rather than
// reporting a clean tree out of an empty corpus.
//
// Hermetic: reads source text off disk and calls a pure function. No disc, no GPU, no window, no
// Core.
#include "testutil.h"

#include "repl_service.h"
#include "config_vars.h"
#include "config.h"
#include "cfg.h"

#include <lucent/config.h>

#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

// ── source corpus ───────────────────────────────────────────────────────────────────────────────
// <this file>/../.. — the psxport checkout under test, not some other copy (tests/CMakeLists.txt
// globs absolute paths, so __FILE__ is absolute). Same derivation as
// tests/test_no_game_address_literals.cpp.
static std::string repo_root(void) {
  std::string f = __FILE__;
  const size_t a = f.rfind('/');
  if (a == std::string::npos) return "";
  const size_t b = f.rfind('/', a - 1);
  if (b == std::string::npos) return "";
  return f.substr(0, b);
}

static std::string read_source(const char* rel, bool* ok) {
  *ok = false;
  const std::string path = repo_root() + "/" + rel;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    fprintf(stderr, "    REFUSING: cannot read %s — this scan searched NOTHING\n", path.c_str());
    return "";
  }
  std::string s;
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  fclose(f);
  *ok = true;
  fprintf(stderr, "    scanned %s: %zu bytes\n", rel, s.size());
  return s;
}

static int count_of(const std::string& hay, const char* needle) {
  int n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
  return n;
}

// ── 1. WIRING: the SBS loop may not ignore stdin ────────────────────────────────────────────────
static void test_sbs_loop_does_not_ignore_stdin(void) {
  bool ok = false;
  const std::string sbs = read_source("runtime/recomp/sbs.cpp", &ok);
  CHECK(ok);            // a missing file is a refusal, not a pass
  CHECK(sbs.size() > 10000);

  const int guard_calls = count_of(sbs, "refuse_if_unserviced");
  const int read_calls  = count_of(sbs, "repl.read(");
  fprintf(stderr, "    sbs.cpp: refusal-guard call sites=%d, repl.read( call sites=%d\n",
          guard_calls, read_calls);
  if (guard_calls + read_calls == 0)
    fprintf(stderr, "    ^ SBS owns the process from PSXPORT_SBS_MODE onward and neither refuses nor\n"
                    "      services stdin: a piped REPL script is silently discarded.\n");
  CHECK(guard_calls + read_calls > 0);
}

// The census that makes the claim above readable rather than a bare assertion: the single-core loop
// is the ONLY REPL pump in the framework, so any OTHER loop that owns the process must refuse.
static void test_repl_pump_census(void) {
  bool ok = false;
  const std::string nb = read_source("runtime/recomp/native_boot.cpp", &ok);
  CHECK(ok);
  const int nb_reads = count_of(nb, "repl.read(");
  fprintf(stderr, "    native_boot.cpp (single-core loop): repl.read( call sites=%d\n", nb_reads);
  CHECK(nb_reads >= 1);      // if this ever hits 0 the REPL has no pump AT ALL and (a) is moot

  bool ok2 = false;
  const std::string rp = read_source("runtime/recomp/repl.cpp", &ok2);
  CHECK(ok2);
  // The single stdin reader in the whole runtime. If a second one appears, this test's premise
  // ("nothing else drains the pipe") needs re-checking, so pin it.
  const int fgets_stdin = count_of(rp, "stdin)");
  fprintf(stderr, "    repl.cpp: fgets(..., stdin) sites=%d\n", fgets_stdin);
  CHECK(fgets_stdin >= 1);
}

// ── 2. POLICY: the decision, exercised on BOTH classes ──────────────────────────────────────────
using psx::repl_service::Query;
using psx::repl_service::Verdict;
using psx::repl_service::decide;
using psx::repl_service::message;

static const char* vname(Verdict v) {
  return v == Verdict::Ok ? "Ok" : v == Verdict::Warn ? "Warn" : "Refuse";
}

// The POSITIVE class: input that IS being dropped must be refused.
static void test_dropped_input_is_refused(void) {
  // The exact repro from kanban #90: a pipe with commands in it, no PSXPORT_REPL set at all.
  Query piped;
  piped.loopServicesRepl = false;
  piped.replRequested = false;
  piped.stdinIsTty = false;
  piped.stdinPendingBytes = 24;      // "newgame\nrun 200\nquit\n"
  fprintf(stderr, "    piped script into a pumpless loop -> %s\n", vname(decide(piped)));
  CHECK(decide(piped) == Verdict::Refuse);

  // An explicit PSXPORT_REPL is refused on its own — no dependence on the timing of the pipe.
  Query asked;
  asked.loopServicesRepl = false;
  asked.replRequested = true;
  asked.stdinPendingBytes = 0;       // nothing written yet
  fprintf(stderr, "    PSXPORT_REPL=1 into a pumpless loop (0 bytes pending) -> %s\n",
          vname(decide(asked)));
  CHECK(decide(asked) == Verdict::Refuse);

  // A file redirect (`binary < script.txt`) is the same defect with a different fd type.
  Query fileIn;
  fileIn.loopServicesRepl = false;
  fileIn.stdinPendingBytes = 512;
  CHECK(decide(fileIn) == Verdict::Refuse);

  // A human typing at a terminal is WARNED, not killed mid-session.
  Query tty;
  tty.loopServicesRepl = false;
  tty.stdinIsTty = true;
  tty.stdinPendingBytes = 5;
  fprintf(stderr, "    keystrokes at a tty into a pumpless loop -> %s\n", vname(decide(tty)));
  CHECK(decide(tty) == Verdict::Warn);
}

// The NEGATIVE class, and it is the half that decides whether this guard is usable at all: every
// legitimate headless launch shape must stay SILENT. A refusal that fires on `< /dev/null` would
// break every existing agent SBS gate (docs/port-framework.md's headless PSXPORT_SBS_MODE=full run)
// and would train operators to ignore the message.
static void test_legitimate_headless_runs_are_not_refused(void) {
  Query devnull;                       // `binary < /dev/null` — readable at EOF, 0 bytes pending
  devnull.loopServicesRepl = false;
  devnull.stdinPendingBytes = 0;
  fprintf(stderr, "    < /dev/null -> %s\n", vname(decide(devnull)));
  CHECK(decide(devnull) == Verdict::Ok);

  Query unknownFd;                     // fd type we cannot measure: "cannot tell" never refuses
  unknownFd.loopServicesRepl = false;
  unknownFd.stdinPendingBytes = -1;
  fprintf(stderr, "    unmeasurable stdin (pending=-1) -> %s\n", vname(decide(unknownFd)));
  CHECK(decide(unknownFd) == Verdict::Ok);

  Query idleTty;                       // interactive SBS, operator not typing
  idleTty.loopServicesRepl = false;
  idleTty.stdinIsTty = true;
  idleTty.stdinPendingBytes = 0;
  CHECK(decide(idleTty) == Verdict::Ok);

  // The single-core loop DOES pump the REPL: same inputs, opposite verdict. Without this case the
  // policy would be a function that says "Refuse" to everything and still passed the class above.
  Query serviced;
  serviced.loopServicesRepl = true;
  serviced.replRequested = true;
  serviced.stdinPendingBytes = 24;
  fprintf(stderr, "    SAME piped script into the single-core (pumping) loop -> %s\n",
          vname(decide(serviced)));
  CHECK(decide(serviced) == Verdict::Ok);
  CHECK(message("native", serviced, Verdict::Ok).empty());   // Ok prints nothing
}

// A refusal that does not say what was dropped and what to use instead is indistinguishable from a
// crash, which is how the original false conclusion survived. Assert the text, not just the verdict.
static void test_the_refusal_names_the_supported_alternative(void) {
  Query piped;
  piped.loopServicesRepl = false;
  piped.replRequested = true;
  piped.stdinIsTty = false;
  piped.stdinPendingBytes = 24;
  const std::string m = message("SBS", piped, decide(piped));
  fprintf(stderr, "    refusal text (%zu chars): %s\n", m.size(), m.c_str());
  CHECK(m.find("SBS") != std::string::npos);                       // which loop
  CHECK(m.find("does NOT service the REPL") != std::string::npos); // what is not serviced
  CHECK(m.find("PSXPORT_REPL") != std::string::npos);              // the knob that was ignored
  CHECK(m.find("24 byte(s)") != std::string::npos);                // the denominator
  CHECK(m.find("PSXPORT_SBS_AUTONAV") != std::string::npos);       // the way to drive an SBS run
  CHECK(m.find("REFUSING THE RUN") != std::string::npos);
  CHECK(m.find("exit 2") != std::string::npos);                    // non-zero, and it says so
  CHECK_EQ(psx::repl_service::kRefusalExit, 2);

  // The warn text must NOT claim it is refusing (it isn't) — a message that lies about what it did
  // is the same defect in the other direction.
  Query tty = piped;
  tty.replRequested = false;
  tty.stdinIsTty = true;
  const std::string w = message("SBS", tty, decide(tty));
  fprintf(stderr, "    warn text (%zu chars): %s\n", w.size(), w.c_str());
  CHECK(w.find("REFUSING THE RUN") == std::string::npos);
  CHECK(w.find("PSXPORT_SBS_AUTONAV") != std::string::npos);
}

// ── 2b. THE SHIPPING FUNCTION ACTUALLY FIRES ────────────────────────────────────────────────────
// A policy nobody calls is the same bug one level up (and so is a probe that measures the wrong
// thing). These cases run the REAL refuse_if_unserviced() — its fstat/FIONREAD probe, its lucent
// line and its exit() — in a forked child with a controlled fd 0, and assert the child's EXIT CODE.
// This is what catches the trap the probe was written around: poll(POLLIN) would report `/dev/null`
// readable at EOF, and case (2) would come back as exit 2 instead of 0.
//
// Hermetic: a pipe, a fork, and a waitpid. No disc, no GPU, no window, no sleep.
enum ChildStdin { kPipeWithScript, kDevNull, kEmptyPipe };

static int child_exit_code(ChildStdin kind, bool servicesRepl, bool replEnv) {
  static const char kScript[] = "newgame\nrun 200\nquit\n";
  int pfd[2] = {-1, -1};
  if (kind != kDevNull && pipe(pfd) != 0) { fprintf(stderr, "    REFUSING: pipe() failed\n"); return -1; }
  if (kind == kPipeWithScript) {
    const ssize_t w = write(pfd[1], kScript, sizeof kScript - 1);
    if (w != (ssize_t)(sizeof kScript - 1)) { fprintf(stderr, "    REFUSING: short write to pipe\n"); return -1; }
  }
  if (kind != kDevNull) close(pfd[1]);   // EOF on the read end once drained — the poll() trap

  const pid_t pid = fork();
  if (pid < 0) { fprintf(stderr, "    REFUSING: fork() failed\n"); return -1; }
  if (pid == 0) {
    int fd = (kind == kDevNull) ? open("/dev/null", O_RDONLY) : pfd[0];
    if (fd < 0) _exit(97);
    if (dup2(fd, STDIN_FILENO) < 0) _exit(98);
    if (replEnv) setenv("PSXPORT_REPL", "1", 1); else unsetenv("PSXPORT_REPL");
    lucent::config::reset_cache();
    psx::config::reset_for_test();
    psx::repl_service::refuse_if_unserviced("SBS-test", servicesRepl);
    _exit(0);                            // it RETURNED: no refusal
  }
  if (kind != kDevNull) close(pfd[0]);
  int status = 0;
  if (waitpid(pid, &status, 0) != pid) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
}

static void test_the_shipping_guard_fires_and_only_when_it_should(void) {
  const int piped = child_exit_code(kPipeWithScript, /*servicesRepl=*/false, /*replEnv=*/false);
  fprintf(stderr, "    child: piped script, pumpless loop            -> exit %d (want 2)\n", piped);
  CHECK_EQ(piped, psx::repl_service::kRefusalExit);

  const int devnull = child_exit_code(kDevNull, false, false);
  fprintf(stderr, "    child: stdin=/dev/null                        -> exit %d (want 0)\n", devnull);
  CHECK_EQ(devnull, 0);

  const int empty = child_exit_code(kEmptyPipe, false, false);
  fprintf(stderr, "    child: closed EMPTY pipe (EOF, 0 bytes)       -> exit %d (want 0)\n", empty);
  CHECK_EQ(empty, 0);

  const int env_only = child_exit_code(kEmptyPipe, false, /*replEnv=*/true);
  fprintf(stderr, "    child: PSXPORT_REPL=1, nothing on stdin       -> exit %d (want 2)\n", env_only);
  CHECK_EQ(env_only, psx::repl_service::kRefusalExit);

  const int serviced = child_exit_code(kPipeWithScript, /*servicesRepl=*/true, /*replEnv=*/true);
  fprintf(stderr, "    child: SAME script, loop DOES pump the REPL   -> exit %d (want 0)\n", serviced);
  CHECK_EQ(serviced, 0);
}

// ── 3. THE KNOB: PSXPORT_REPL resolves through the CVar ladder, unchanged ────────────────────────
// The guard reads PSXPORT_REPL, so the knob was migrated onto a CVar (config_vars.h cv_repl) rather
// than read raw. The point of a migration is that it cannot change what a run does — so check the
// migrated value against the pre-migration accessor, in both states.
static void test_psxport_repl_is_a_ladder_knob_matching_its_pre_migration_behaviour(void) {
  CHECK(psx::config::find("PSXPORT_REPL") != nullptr);   // it is IN the inventory, not read raw

  // The oracle is the pre-migration body: native_boot.cpp read cfg_on("PSXPORT_REPL"), and cfg_on
  // used to be `lucent::config::flag(name) ? 1 : 0`. Same env, same answer, or the knob's meaning
  // moved under the migration. Same value set as tests/test_config_cvar.cpp uses.
  const char* const values[] = {nullptr, "1", "0", "yes", "off", "", "banana"};
  int compared = 0;
  for (const char* v : values) {
    if (v) setenv("PSXPORT_REPL", v, 1); else unsetenv("PSXPORT_REPL");
    lucent::config::reset_cache();       // lucent memoises per name
    psx::config::reset_for_test();       // a CVar binds its env Override once
    const int want = lucent::config::flag("PSXPORT_REPL") ? 1 : 0;
    const int got  = psx::config::cv_repl.get() ? 1 : 0;
    fprintf(stderr, "    PSXPORT_REPL=%-8s cv_repl=%d  pre-migration=%d\n",
            v ? (*v ? v : "\"\"") : "<unset>", got, want);
    CHECK_EQ(got, want);
    ++compared;
  }
  fprintf(stderr, "    compared %d value(s) against the pre-migration implementation\n", compared);
  CHECK_EQ(compared, 7);
  unsetenv("PSXPORT_REPL");
  lucent::config::reset_cache();
  psx::config::reset_for_test();
}

int main(void) {
  RUN(sbs_loop_does_not_ignore_stdin);
  RUN(repl_pump_census);
  RUN(dropped_input_is_refused);
  RUN(legitimate_headless_runs_are_not_refused);
  RUN(the_refusal_names_the_supported_alternative);
  RUN(the_shipping_guard_fires_and_only_when_it_should);
  RUN(psxport_repl_is_a_ladder_knob_matching_its_pre_migration_behaviour);
  return pt_summary();
}

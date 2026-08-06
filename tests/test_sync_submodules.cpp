// test_sync_submodules.cpp — scripts/sync-submodules.sh must never certify what it did not look at.
//
// WHY THIS TEST EXISTS
// --------------------
// `git submodule status --recursive` and `git submodule foreach --recursive` both ABORT partway
// through every repo in this workspace: vendor/beetle-psx registers a nested `deps/lightning/gnulib`
// with no URL in .gitmodules, git gives up there, and NEVER REACHES vendor/lucent.
//
//     fatal: no submodule mapping found in .gitmodules for path 'deps/lightning/gnulib'
//     fatal: failed to recurse into submodule 'vendor/beetle-psx'
//
// The script ran both under `|| true`, so the non-zero exit vanished and the truncated list was
// reported as a clean bill of health — measured on the real trees:
//
//     Tomba2Engine, old script: "[submodules] 2 submodule(s) checked, all at this repo's recorded
//                                gitlinks"        (it declares 3)
//     spyro,        old script: 3 of 4.
//
// It was harmless only while vendor/lucent happened to sit on its gitlink. When it did not, three
// builds died in one day: ot_attr.h needs lucent >= 07c5836 and the checkout was at 02ea34b, while
// the sync step said everything matched.
//
// WHY A C++ FILE FOR A SHELL SCRIPT
// ---------------------------------
// Because tests/CMakeLists.txt globs `test_*.c` / `test_*.cpp` into ctest and nothing else, so a
// `.sh` here would be a file nobody runs — which is the same defect one level up. This binary is a
// driver: it builds real throwaway git repositories in its own working directory and runs the real
// script against them. Nothing here links or includes framework code.
//
// HERMETIC: no network (every remote is a local path), no disc, no GPU, no wall-clock sleeps. Each
// fixture is a fresh directory under the test's cwd; it is deleted when its case passes and LEFT
// BEHIND when the case fails, because that tree is the evidence.
//
// THE FIXTURE IS THE WORKSPACE IN MINIATURE
//
//     game/                              <- sync-submodules.sh runs here (like spyro/)
//       external/framework/              <- like external/psxport
//         vendor/beetle/                 <- like vendor/beetle-psx
//           deps/lightning/gnulib        <- a gitlink in the tree, NOT in .gitmodules: THE POISON
//         vendor/lucent/                 <- like vendor/lucent: the path git's recursion never reaches
//         vendor/ghost/                  <- (case 2 only) declared + recorded, but unclonable
//
// Case 1 asserts the fixture is genuinely poisoned before it asserts anything about the script, so a
// green run cannot mean "the blind spot did not reproduce".

#include "testutil.h"

#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

/* ---- shelling out ------------------------------------------------------------------------------ */

struct Run {
  int rc = -1;
  std::string out;
};

static Run run(const std::string& cmd) {
  Run r;
  FILE* p = popen((cmd + " 2>&1").c_str(), "r");
  if (!p) {
    r.out = "popen failed";
    return r;
  }
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, p)) > 0) r.out.append(buf, n);
  const int st = pclose(p);
  r.rc = (st == -1) ? -1 : (WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st));
  return r;
}

static std::string capture(const std::string& cmd) {
  std::string s = run(cmd).out;
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

static bool has(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

/* ---- locating the script under test ----------------------------------------------------------- */
/* __FILE__ is what CMake passed the compiler, and tests/CMakeLists.txt feeds it an ABSOLUTE path
 * (file(GLOB) yields absolute paths), so <this file>/../.. is the psxport checkout being tested —
 * the one whose script must be exercised, not some other copy found on PATH. If that assumption ever
 * breaks, this fails loudly with the path it computed rather than testing nothing. */
static std::string script_path(void) {
  std::string f = __FILE__;
  const size_t a = f.rfind('/');
  if (a == std::string::npos) return "";
  const size_t b = f.rfind('/', a - 1);
  if (b == std::string::npos) return "";
  return f.substr(0, b) + "/scripts/sync-submodules.sh";
}

/* ---- fixture ----------------------------------------------------------------------------------- */

struct Fixture {
  std::string root;  /* absolute; holds src/ (the "remotes"), game/ (the checkout), home/ */
  std::string env;   /* env prefix that isolates git from the developer's own config       */
  std::string git;   /* `git` plus the flags every fixture command needs                   */
  bool ok = false;
};

static bool step(const Fixture& fx, const std::string& cmd, bool required = true) {
  const Run r = run(fx.env + cmd);
  if (required && r.rc != 0) {
    fprintf(stderr, "    fixture step FAILED rc=%d: %s\n      %s\n", r.rc, cmd.c_str(),
            r.out.c_str());
    return false;
  }
  return true;
}

/* ghost      = declare a submodule whose remote does not exist (an UNRESOLVABLE declared path)
 * arm_drift  = leave vendor/lucent one commit BEHIND its recorded gitlink (stale — sync should fix)
 * ahead_drift= leave vendor/lucent one commit AHEAD of it (a DELIBERATE checkout — must be kept)
 * local_work = leave an untracked file inside vendor/lucent                                        */
static Fixture make_fixture(const char* name, bool ghost, bool arm_drift, bool local_work,
                           bool ahead_drift = false) {
  Fixture fx;
  char cwd[4096];
  if (!getcwd(cwd, sizeof cwd)) return fx;
  fx.root = std::string(cwd) + "/sync_submodules_fixture_" + name;

  run("rm -rf '" + fx.root + "'");
  if (run("mkdir -p '" + fx.root + "/home'").rc != 0) return fx;
  run("touch '" + fx.root + "/home/.gitconfig'");

  fx.env = "env HOME='" + fx.root + "/home' GIT_CONFIG_GLOBAL='" + fx.root +
           "/home/.gitconfig' GIT_CONFIG_NOSYSTEM=1 GIT_TERMINAL_PROMPT=0 "
           "GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@t ";
  fx.git = "git -c init.defaultBranch=main -c protocol.file.allow=always -c advice.detachedHead=false";

  const std::string R = "'" + fx.root + "'";
  const std::string G = fx.git;
  auto mk = [&](const char* rel) {
    return "mkdir -p " + R + "/" + rel + " && " + G + " -C " + R + "/" + rel + " init -q && echo " +
           rel + " > " + R + "/" + rel + "/README && " + G + " -C " + R + "/" + rel +
           " add -A && " + G + " -C " + R + "/" + rel + " commit -qm c1";
  };

  std::vector<std::string> steps;
  steps.push_back(mk("src/lucent"));
  /* lucent needs two commits so "one commit behind the gitlink" is expressible */
  steps.push_back("echo two > " + R + "/src/lucent/README && " + G + " -C " + R +
                  "/src/lucent commit -qam c2");
  steps.push_back(mk("src/beetle"));
  for (const std::string& s : steps)
    if (!step(fx, s)) return fx;

  /* THE POISON: a gitlink in beetle's tree with no .gitmodules mapping — beetle-psx's
   * deps/lightning/gnulib, reproduced exactly. Any git recursion dies here. */
  const std::string bl = capture(fx.env + G + " -C " + R + "/src/lucent rev-parse HEAD");
  if (bl.size() != 40) {
    fprintf(stderr, "    fixture: could not read a sha for the poison gitlink (got \"%s\")\n",
            bl.c_str());
    return fx;
  }
  if (!step(fx, G + " -C " + R + "/src/beetle update-index --add --cacheinfo 160000," + bl +
                    ",deps/lightning/gnulib")) return fx;
  if (!step(fx, G + " -C " + R + "/src/beetle commit -qm poison")) return fx;

  if (!step(fx, mk("src/framework"))) return fx;
  if (!step(fx, G + " -C " + R + "/src/framework submodule add -q ../beetle vendor/beetle")) return fx;
  if (!step(fx, G + " -C " + R + "/src/framework submodule add -q ../lucent vendor/lucent")) return fx;
  if (ghost) {
    if (!step(fx, mk("src/ghost"))) return fx;
    if (!step(fx, G + " -C " + R + "/src/framework submodule add -q ../ghost vendor/ghost")) return fx;
  }
  if (!step(fx, G + " -C " + R + "/src/framework commit -qm vendor")) return fx;
  if (ghost) /* delete the remote AFTER recording the gitlink: declared + recorded + unclonable */
    if (!step(fx, "rm -rf " + R + "/src/ghost " + R + "/src/framework/vendor/ghost")) return fx;

  if (!step(fx, mk("src/game"))) return fx;
  if (!step(fx, G + " -C " + R + "/src/game submodule add -q ../framework external/framework")) return fx;
  if (!step(fx, G + " -C " + R + "/src/game commit -qm vendor")) return fx;
  if (!step(fx, G + " clone -q " + R + "/src/game " + R + "/game")) return fx;
  /* This is EXPECTED to report failure — it is the poison doing its job. */
  step(fx, G + " -C " + R + "/game submodule update --init --recursive", false);

  if (arm_drift) {
    const std::string old = capture(fx.env + G + " -C " + R + "/src/lucent rev-parse HEAD~1");
    if (old.size() != 40) return fx;
    if (!step(fx, G + " -C " + R + "/game/external/framework/vendor/lucent checkout -q " + old))
      return fx;
  }
  /* AHEAD: a commit the recorded gitlink does not contain, checked out on purpose. This is the
   * operator bumping a pin, or an agent testing a newer framework — the exact case a blind
   * `git submodule update` silently destroys. */
  if (ahead_drift) {
    if (!step(fx, G + " -C " + R + "/src/lucent commit -q --allow-empty -m ahead")) return fx;
    const std::string tip = capture(fx.env + G + " -C " + R + "/src/lucent rev-parse HEAD");
    if (tip.size() != 40) return fx;
    if (!step(fx, G + " -C " + R + "/game/external/framework/vendor/lucent fetch -q origin")) return fx;
    if (!step(fx, G + " -C " + R + "/game/external/framework/vendor/lucent checkout -q " + tip)) return fx;
  }
  if (local_work)
    if (!step(fx, "echo mine > " + R + "/game/external/framework/vendor/lucent/LOCAL_WORK")) return fx;

  fx.ok = true;
  return fx;
}

static std::string lucent_head(const Fixture& fx) {
  return capture(fx.env + fx.git + " -C '" + fx.root +
                 "/game/external/framework/vendor/lucent' rev-parse HEAD");
}
static std::string lucent_gitlink(const Fixture& fx) {
  return capture(fx.env + fx.git + " -C '" + fx.root +
                 "/game/external/framework' ls-files -s -- vendor/lucent | awk '{print $2}'");
}
/* popen() has no cwd argument, so the `cd` is part of the command line. */
static Run script_in(const Fixture& fx) {
  return run("cd '" + fx.root + "/game' && " + fx.env + "bash '" + script_path() + "'");
}

static void cleanup(const Fixture& fx) { run("rm -rf '" + fx.root + "'"); }

/* ---- case 0: the harness itself ---------------------------------------------------------------- */
/* If git is missing or the script is not where this test computed it to be, every other case below
 * would "pass" by never exercising anything. That must be a red test, not a quiet one. */
static void test_preconditions(void) {
  const std::string sp = script_path();
  fprintf(stderr, "  script under test: %s\n", sp.c_str());
  CHECK(!sp.empty() && sp[0] == '/');
  CHECK_EQ(run("test -f '" + sp + "'").rc, 0);
  const Run g = run("git --version");
  fprintf(stderr, "  %s", g.out.c_str());
  CHECK_EQ(g.rc, 0);
}

/* ---- case 1: THE DEFECT ------------------------------------------------------------------------ */
static void test_does_not_certify_a_submodule_gits_recursion_never_reached(void) {
  Fixture fx = make_fixture("drift", /*ghost=*/false, /*arm_drift=*/true, /*local_work=*/false);
  CHECK(fx.ok);

  /* (a) The blind spot is REAL in this fixture: git's own recursion aborts and lists 2 of the 3
   *     declared submodules, vendor/lucent among the missing. Asserted, not assumed — otherwise a
   *     green result could just mean the fixture failed to reproduce the trap. */
  const Run st = run("cd '" + fx.root + "/game' && " + fx.env + "git submodule status --recursive");
  const std::string listed =
      capture("cd '" + fx.root + "/game' && " + fx.env +
              "git submodule status --recursive 2>/dev/null | grep -c . || true");
  fprintf(stderr, "  git submodule status --recursive listed %s of the 3 declared (rc=%d):\n%s",
          listed.c_str(), st.rc, st.out.c_str());
  CHECK_STREQ(listed.c_str(), "2");
  CHECK(st.rc != 0);
  CHECK(!has(st.out, "vendor/lucent"));
  CHECK(has(st.out, "vendor/beetle"));

  /* (b) And vendor/lucent is genuinely off its recorded gitlink. */
  const std::string before = lucent_head(fx), pin = lucent_gitlink(fx);
  fprintf(stderr, "  vendor/lucent: checkout=%s recorded=%s\n", before.c_str(), pin.c_str());
  CHECK(before.size() == 40 && pin.size() == 40);
  CHECK(before != pin);

  /* (c) The script must not report a clean bill of health, and must actually fix the pin.
   *     BEFORE THE FIX this printed "2 submodule(s) checked, all at this repo's recorded gitlinks",
   *     exited 0, and left the checkout where it was. */
  const Run r = script_in(fx);
  fprintf(stderr, "  script exit=%d, output:\n%s", r.rc, r.out.c_str());
  CHECK(!has(r.out, "2 submodule(s) checked, all at"));
  const std::string after = lucent_head(fx);
  fprintf(stderr, "  vendor/lucent after: %s (want %s)\n", after.c_str(), pin.c_str());
  CHECK_EQ(r.rc, 0);
  CHECK(after == pin);
  cleanup(fx);
}

/* ---- case 2: it REFUSES when it cannot see a declared submodule -------------------------------- */
static void test_refuses_to_certify_when_a_declared_submodule_is_unseeable(void) {
  Fixture fx = make_fixture("ghost", /*ghost=*/true, /*arm_drift=*/false, /*local_work=*/false);
  CHECK(fx.ok);

  /* `cd` is a shell builtin, so it must precede the `env` prefix, never follow it. */
  const std::string declared =
      capture("cd '" + fx.root + "/game' && " + fx.env +
              "git config -f external/framework/.gitmodules --get-regexp "
              "'^submodule\\..*\\.path$' | grep -c . || true");
  fprintf(stderr, "  framework declares %s submodules (+1 top-level); vendor/ghost has no clonable "
                  "remote, so 4 are declared in total\n",
          declared.c_str());
  CHECK_STREQ(declared.c_str(), "3");

  const Run r = script_in(fx);
  fprintf(stderr, "  script exit=%d, output:\n%s", r.rc, r.out.c_str());
  CHECK(r.rc != 0);                       /* REFUSE, loudly */
  CHECK(has(r.out, "CANNOT SEE"));
  CHECK(has(r.out, "vendor/ghost"));      /* names WHAT it could not see */
  CHECK(has(r.out, " of 4 submodule(s)"));/* carries its DENOMINATOR */
  CHECK(!has(r.out, "all at this repo's recorded gitlinks"));
  cleanup(fx);
}

/* ---- case 3: the OTHER class — a genuinely in-sync tree ---------------------------------------- */
/* The negative control for case 1. A script that refused unconditionally, or that could not see
 * vendor/lucent at all, would fail here — this is what proves the check discriminates rather than
 * just always saying no. It also pins the denominator: 3, the number the repo DECLARES, not 2, the
 * number git's broken recursion manages to list. */
static void test_certifies_an_in_sync_tree_with_the_full_denominator(void) {
  Fixture fx = make_fixture("insync", /*ghost=*/false, /*arm_drift=*/false, /*local_work=*/false);
  CHECK(fx.ok);
  CHECK(lucent_head(fx) == lucent_gitlink(fx));

  const Run r = script_in(fx);
  fprintf(stderr, "  script exit=%d, output:\n%s", r.rc, r.out.c_str());
  CHECK_EQ(r.rc, 0);
  CHECK(has(r.out, "checked 3 of 3 submodule(s), all at this repo's recorded gitlinks"));
  /* …and the verdict carries its BLIND SPOT. 3 of 3 is only meaningful alongside what the 3 does not
   * include: the gitlink no .gitmodules declares, which git cannot sync and which is the very thing
   * that aborts its recursion. A count printed without it would be true and still misleading. */
  CHECK(has(r.out, "vendor/beetle/deps/lightning/gnulib"));
  cleanup(fx);
}

/* ---- case 4: the dirt guard still protects local work ------------------------------------------ */
/* The guard now asks each submodule for its status with --ignore-submodules=all, because a parent
 * whose NESTED submodule is off its pin reports ` M vendor/lucent` — pointer drift, not local work,
 * and treating it as dirt made the script decline to sync in exactly the case it exists for (case 1
 * fails without that flag). This case is the other half of that discriminator: a real untracked file
 * in the SAME checkout must still block the sync and be named. */
static void test_still_refuses_to_clobber_real_local_work(void) {
  Fixture fx = make_fixture("dirty", /*ghost=*/false, /*arm_drift=*/true, /*local_work=*/true);
  CHECK(fx.ok);
  const std::string before = lucent_head(fx);
  CHECK(before != lucent_gitlink(fx));

  const Run r = script_in(fx);
  fprintf(stderr, "  script exit=%d, output:\n%s", r.rc, r.out.c_str());
  CHECK(has(r.out, "NOT syncing"));
  CHECK(has(r.out, "external/framework/vendor/lucent"));
  CHECK(lucent_head(fx) == before);  /* the local work is still there, unclobbered */
  CHECK_EQ(run("test -f '" + fx.root + "/game/external/framework/vendor/lucent/LOCAL_WORK'").rc, 0);
  cleanup(fx);
}

/* ---- case 5: a DELIBERATE checkout is never rewound -------------------------------------------- */
/* The sync exists for ONE direction: the recorded gitlink moved and the checkout is BEHIND it (case
 * 1). Running it in the other direction destroys work: MEASURED 2026-08-06, Tomba2Engine was checked
 * out onto psxport 9a08efca, built and gated green, and a CONCURRENT agent's ./run.sh reverted it to
 * the recorded 9890eaa8 — the following commit recorded the OLD pin and only a `git ls-tree` caught
 * it. Reporting the move was not enough; by then the checkout is gone.
 *
 * Ancestry is the discriminator. This case pins the DESCENDANT arm; case 1 pins the ANCESTOR arm, so
 * the two together prove the script distinguishes them rather than just refusing everything. */
static void test_never_rewinds_a_deliberate_checkout(void) {
  Fixture fx = make_fixture("ahead", /*ghost=*/false, /*arm_drift=*/false, /*local_work=*/false,
                            /*ahead_drift=*/true);
  CHECK(fx.ok);
  const std::string before = lucent_head(fx);
  CHECK(before != lucent_gitlink(fx));

  const Run r = script_in(fx);
  fprintf(stderr, "  script exit=%d, output:\n%s", r.rc, r.out.c_str());
  CHECK(has(r.out, "NOT syncing"));
  CHECK(has(r.out, "AHEAD of the recorded gitlink"));
  CHECK(has(r.out, "external/framework/vendor/lucent"));
  /* THE ASSERTION THAT MATTERS: the checkout survived. */
  CHECK(lucent_head(fx) == before);
  cleanup(fx);
}

int main(void) {
  RUN(preconditions);
  RUN(does_not_certify_a_submodule_gits_recursion_never_reached);
  RUN(refuses_to_certify_when_a_declared_submodule_is_unseeable);
  RUN(certifies_an_in_sync_tree_with_the_full_denominator);
  RUN(still_refuses_to_clobber_real_local_work);
  RUN(never_rewinds_a_deliberate_checkout);
  return pt_summary();
}

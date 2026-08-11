// test_zero_config_is_loud.cpp — THE NEGATIVE PATH of the GameConfig sweep: does an un-RE'd field
// actually make the framework SPEAK, and does a filled one make it STAY QUIET?
//
// WHY THIS FILE EXISTS, AND WHY IT IS NOT test_render_noise_mask.cpp'S JOB.
// test_render_noise_mask.cpp asserts the mask's ARITHMETIC — that `known` is false and `covers()` is
// empty on a zero GameConfig. That is necessary and it is not sufficient: an empty mask that says
// NOTHING is exactly the failure the whole sweep exists to kill. "No render region masked" and "I was
// never given one" print the same in a log that prints neither, and a harness reporting no divergence
// over an unmasked region is indistinguishable from a harness that found none. So this file asserts
// the OBSERVABLE OUTPUT: it installs a lucent sink and checks the real emitted lines.
//
// It also asserts the FALSE-ALARM direction, which nothing else did. A warning that fires on the one
// game that HAS filled these fields is not a harmless extra line — it is how an operator learns that
// these warnings are noise, and after that the loud path is decoration. Silence on Tomba!2 is a
// requirement, so it is a check here.
//
// ORDER IS LOAD-BEARING, and that is itself a finding this file pins (see test_second_consumer...):
// RenderNoiseMask::from() warns AT MOST ONCE PER PROCESS per kind (function-local statics in an
// inline function = one object across the program). So the silent case must run FIRST — if it ran
// after the zero case it would "pass" only because the warning had already been spent. RUN() order in
// main() is therefore fixed, and the comment there says so.
//
// WHAT A NEGATIVE PRINTS HERE: every case asserts the captured LINE COUNT, not just the presence of a
// substring, and dumps whatever it captured on failure. "0 lines" (the diagnostic never fired) and "3
// lines" (it fires on a correctly configured game too) are each distinguishable from a pass, in both
// directions.
//
// Hermetic: GameConfig is a POD, render_noise.h/task_slot_layout.h are pure arithmetic over it, and
// lucent's sink is in-process. No Core, no GPU, no disc, no window.
#include "testutil.h"

#include "render_noise.h"
#include "task_slot_layout.h"

#include <lucent/log.h>

#include <string>
#include <vector>

// ── the capture sink ────────────────────────────────────────────────────────────────────────────
static std::vector<std::string> g_lines;

static void capture_start(void) {
  g_lines.clear();
  lucent::set_sink([](lucent::Level, std::string_view line) { g_lines.emplace_back(line); });
}
static void capture_stop(void) { lucent::set_sink(nullptr); }

// Dump what was captured, so a wrong COUNT is debuggable from the test output alone.
static void dump_capture(const char* what) {
  fprintf(stderr, "  [%s] captured %zu line(s)\n", what, g_lines.size());
  for (const std::string& l : g_lines) fprintf(stderr, "  [%s] | %s\n", what, l.c_str());
}

static bool line_has(size_t i, const char* needle) {
  return i < g_lines.size() && g_lines[i].find(needle) != std::string::npos;
}
// Does ANY captured line contain `needle`? Used for the must-NOT-appear checks, where "not in line 0"
// would be a weaker claim than intended.
static bool any_line_has(const char* needle) {
  for (const std::string& l : g_lines)
    if (l.find(needle) != std::string::npos) return true;
  return false;
}

// ── the two configs ────────────────────────────────────────────────────────────────────────────
// Tomba!2's real values — Tomba2Engine/game/core/game_config.cpp, the only game that has RE'd these.
static GameConfig tomba2_cfg(void) {
  GameConfig c{};
  c.packetPoolBase   = 0x800BFE68u;
  c.packetPoolStride = 0x00014000u;
  c.otRegionBase     = 0x800E80A8u;
  c.otRegionStride   = 0x00002070u;
  c.poolPtrCur       = 0x800BF544u;
  c.poolPtrLast      = 0x800BF4F4u;
  c.dwellCounter     = 0x800E809Cu;
  c.taskTableBase    = 0x801FE000u;
  c.taskSlotStride   = 0x00000070u;
  c.taskCount        = 3u;
  c.curTaskPtr       = 0x1F800138u;
  c.stageGame        = 0x8010637Cu;
  return c;
}
// spyro / spider1: every field above is an HONEST ZERO with a TODO in their game_config.cpp. A
// default-constructed GameConfig IS that state, which is why the zero cases below construct one
// rather than copying their files.
static GameConfig unre_cfg(void) { return GameConfig{}; }

// ════════════════════════════════════════════════════════════════════════════════════════════════
// CASE 1 (MUST RUN FIRST): the false-alarm direction. On the game that filled the fields, building
// the mask emits NOTHING — not a warn, not an info, nothing. Runs before any zero case so that a pass
// here cannot be an artifact of a once-per-process warning already having been spent.
// ════════════════════════════════════════════════════════════════════════════════════════════════
static void test_filled_config_is_silent(void) {
  const GameConfig cfg = tomba2_cfg();
  capture_start();
  const RenderNoiseMask m = RenderNoiseMask::from(&cfg, "test-filled");
  // Two more builds, because a per-call (rather than once-per-process) warn would only show up on a
  // repeat if the first were the one being suppressed.
  const RenderNoiseMask m2 = RenderNoiseMask::from(&cfg, "test-filled-again");
  const RenderNoiseMask m3 = RenderNoiseMask::from(&cfg, "test-filled-third");
  capture_stop();
  if (!g_lines.empty()) dump_capture("filled");
  CHECK_EQ((long long)g_lines.size(), 0);   // the whole point: a correct game hears nothing
  // …and it is silent because it SUCCEEDED, not because it bailed out early.
  CHECK(m.known);
  CHECK(m2.known);
  CHECK(m3.known);
  CHECK_EQ(m.bytes(), (0x800E7E68u - 0x800BFE68u) + (0x800EC188u - 0x800E80A8u) +
                          (0x800E80A8u - 0x800E7E68u) + (0x800BF54Cu - 0x800BF4F0u));
  // The report header must print THIS run's real window, never the "unset" apology.
  char buf[256];
  const char* d = m.describe(buf, sizeof buf);
  CHECK(std::string(d).find("unset") == std::string::npos);
  CHECK(std::string(d).find("0x800BFE68") != std::string::npos);
  CHECK(std::string(d).find("0x800EC188") != std::string::npos);
}

// The task-table derivations are equally silent and equally correct on the filled config. Same file as
// the loud half on purpose: the two directions of one predicate belong in one place.
static void test_filled_config_task_addrs_are_the_literals(void) {
  const GameConfig cfg = tomba2_cfg();
  capture_start();
  const uint32_t entry = task0_stage_entry_addr(&cfg);
  const uint32_t sm    = task0_state_mach_addr(&cfg);
  const uint32_t s4a   = task0_field_submode_addr(&cfg);
  const uint32_t s4e   = task0_field_runstate_addr(&cfg);
  capture_stop();
  if (!g_lines.empty()) dump_capture("filled-task");
  CHECK_EQ((long long)g_lines.size(), 0);
  CHECK_EQ(entry, 0x801FE00Cu);   // sbs.cpp's old TASK0_ENTRY
  CHECK_EQ(sm,    0x801FE048u);   // the vabBuildPending / MODE=skip gate word
  CHECK_EQ(s4a,   0x801FE04Au);   // old SM_S4A
  CHECK_EQ(s4e,   0x801FE04Eu);   // old SM_S4E
  // sbs.cpp's nav predicate, spelled exactly as Impl::navArm() spells it. TRUE here means auto-nav
  // arms on Tomba!2 — i.e. the refusal below is not achieved by breaking the working game.
  CHECK(entry && cfg.stageGame);
  // MODE=skip's startup guard is `!mStageSmAddr`: it must NOT refuse on Tomba!2.
  CHECK(sm != 0u);
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// CASE 2: a PARTIAL config still announces which half is missing. Runs before the all-zero case
// because the two warnings are separate once-per-process statics and this one is the narrower claim.
// ════════════════════════════════════════════════════════════════════════════════════════════════
static void test_partial_config_names_the_missing_parts(void) {
  GameConfig cfg{};
  cfg.packetPoolBase = 0x80100000u; cfg.packetPoolStride = 0x1000u;   // pool known; OT and ptrs not
  capture_start();
  const RenderNoiseMask m = RenderNoiseMask::from(&cfg, "test-partial");
  RenderNoiseMask::from(&cfg, "test-partial-again");                   // once per process, so: 1 line
  capture_stop();
  dump_capture("partial");
  CHECK_EQ((long long)g_lines.size(), 1);
  CHECK(line_has(0, "PARTIAL"));
  CHECK(line_has(0, "test-partial"));         // names WHICH consumer's verdict is affected
  CHECK(line_has(0, "ordering table MISSING"));
  CHECK(line_has(0, "pool pointers MISSING"));
  CHECK(line_has(0, "packet pool ok"));       // …and does not claim the part it DOES have is missing
  // A partial mask must not silently inherit the rest.
  CHECK(m.known);
  CHECK_EQ(m.otLo, 0u);
  CHECK_EQ(m.ptrLo, 0u);
  CHECK(!m.covers(0x800E80A8u));              // Tomba!2's OT is NOT masked for this game
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// CASE 3: THE ALL-ZERO CONFIG — spyro / spider1 today. The mask is empty AND it says so, once,
// at warn level, naming the fields to fill; and the text contains none of Tomba!2's addresses,
// because printing them is how an inherited window gets read as this run's.
// ════════════════════════════════════════════════════════════════════════════════════════════════
static void test_zero_config_warns_loudly(void) {
  const GameConfig cfg = unre_cfg();
  capture_start();
  const RenderNoiseMask m = RenderNoiseMask::from(&cfg, "test-zero");
  capture_stop();
  dump_capture("zero");
  CHECK_EQ((long long)g_lines.size(), 1);     // it FIRED — the check this whole file exists for
  CHECK(line_has(0, "render-noise"));         // on a channel, so it is greppable
  CHECK(line_has(0, "test-zero"));            // names the affected consumer
  CHECK(line_has(0, "EMPTY"));
  CHECK(line_has(0, "packetPool"));           // names what to fill, not just that something is wrong
  CHECK(line_has(0, "otRegion"));
  CHECK(line_has(0, "REPORTED AS GAMEPLAY DIVERGENCE"));   // names the CONSEQUENCE for the verdict
  // It must not print another game's window as if it were a fallback.
  CHECK(!any_line_has("0x800BFE68"));
  CHECK(!any_line_has("800bfe68"));
  CHECK(!m.known);
  CHECK_EQ(m.bytes(), 0u);
  // The blindness must be visible in a REPORT HEADER too, not only in the log stream — a harness
  // report read after the fact is where the wrong-region mistake actually gets made.
  char buf[256];
  const char* d = m.describe(buf, sizeof buf);
  CHECK(std::string(d).find("unset") != std::string::npos);
  CHECK(std::string(d).find("0x800BFE68") == std::string::npos);
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// CASE 4: the once-per-process budget, asserted rather than assumed — a KNOWN BLIND SPOT, pinned so
// it cannot regress further and cannot be discovered the hard way. A second consumer building the
// same empty mask gets NO line of its own: in a real spyro process ot_attr.cpp's pool_range() runs on
// the render path and spends the warning, so sbs.cpp's "sbs-addrlabel" blindness is never named. The
// process still learns the fields are unset (once), which is why this is a blind spot and not a
// silent failure — but the consumer list in the log is NOT the list of affected consumers.
// ════════════════════════════════════════════════════════════════════════════════════════════════
static void test_second_consumer_of_an_empty_mask_is_not_named(void) {
  const GameConfig cfg = unre_cfg();
  capture_start();
  const RenderNoiseMask a = RenderNoiseMask::from(&cfg, "second-consumer-sbs-addrlabel");
  const RenderNoiseMask b = RenderNoiseMask::from(nullptr, "third-consumer-dualcore");
  capture_stop();
  dump_capture("second-consumer");
  CHECK_EQ((long long)g_lines.size(), 0);            // documented, not desired
  CHECK(!any_line_has("second-consumer-sbs-addrlabel"));
  // The MASK is still correct for both — the suppression is of the announcement only, so a consumer
  // that checks `known` (as every one of them does) still refuses/labels honestly.
  CHECK(!a.known);
  CHECK(!b.known);
  CHECK(!a.covers(0x800BFE68u));
  CHECK(!b.covers(0x801FE000u));
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// CASE 5: the task-table derivations on a zero config yield 0 — NOT a small offset that reads as a
// valid address. This is the difference between refusing and reading BIOS: mem_r32(0x0C) answers, and
// the answer is a lie. Each derived address here is one sbs.cpp site's guard condition.
// ════════════════════════════════════════════════════════════════════════════════════════════════
static void test_zero_config_task_addrs_are_zero_not_offsets(void) {
  const GameConfig cfg = unre_cfg();
  CHECK_EQ(task0_stage_entry_addr(&cfg),    0u);   // NOT 0x0C — sbs kP[] "stage" probe / sbs-ww stage=
  CHECK_EQ(task0_state_mach_addr(&cfg),     0u);   // NOT 0x48 — MODE=skip's vabBuildPending gate
  CHECK_EQ(task0_field_submode_addr(&cfg),  0u);   // NOT 0x4A — navStep s4a
  CHECK_EQ(task0_field_runstate_addr(&cfg), 0u);   // NOT 0x4E — navStep s4e
  CHECK_EQ(task_slot_base(&cfg, 0),         0u);
  CHECK_EQ(task_slot_base(&cfg, 2),         0u);   // NOT 0xE0 — the task-slot dump's third row
  // Every one of the four helpers must also survive a null config (a game with no GameConfig at all).
  CHECK_EQ(task0_stage_entry_addr(nullptr),    0u);
  CHECK_EQ(task0_state_mach_addr(nullptr),     0u);
  CHECK_EQ(task0_field_submode_addr(nullptr),  0u);
  CHECK_EQ(task0_field_runstate_addr(nullptr), 0u);
  CHECK_EQ(task_slot_base(nullptr, 1),         0u);
  // sbs.cpp's guards, spelled as sbs.cpp spells them, asserted FALSE:
  //   navArm():        mNavKnown = mNavEntryAddr && mNavStageGame   -> no auto-nav
  CHECK(!(task0_stage_entry_addr(&cfg) && cfg.stageGame));
  //   run():           mMode == M_SKIP && !mStageSmAddr             -> refuse MODE=skip
  CHECK(!task0_state_mach_addr(&cfg));
  //   stagetrace:      curTaskPtr && stage-entry                    -> trace disabled
  CHECK(!(cfg.curTaskPtr && task0_stage_entry_addr(&cfg)));
  //   task-slot dump:  taskTableBase ? taskCount : 0                -> dumps nothing
  CHECK_EQ((cfg.taskTableBase ? cfg.taskCount : 0u), 0u);
  //   addrLabel:       taskTableBase && taskCount                   -> no "task_slots" label
  CHECK(!(cfg.taskTableBase && cfg.taskCount));
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// CASE 6: THE PARTIAL-CONFIG TRAP that makes the nav predicate a two-field AND rather than one. A
// game that fills taskTableBase but has not found its stage-entry PC (or vice versa) must NOT arm
// auto-nav: with stageGame 0 the REACH_GAME test is `mem_r32(entry) == 0`, and the stage word IS 0
// during boot, so nav would declare "GAME @f0" and gate a byte-compare on the BIOS boot. Half-filled
// is the state a game passes THROUGH while being RE'd, so this is the likely case, not the exotic one.
// ════════════════════════════════════════════════════════════════════════════════════════════════
static void test_half_filled_config_does_not_arm_nav(void) {
  GameConfig onlyTable{};
  onlyTable.taskTableBase = 0x801FE000u; onlyTable.taskSlotStride = 0x70u; onlyTable.taskCount = 3u;
  CHECK(task0_stage_entry_addr(&onlyTable) != 0u);              // the address IS derivable…
  CHECK(!(task0_stage_entry_addr(&onlyTable) && onlyTable.stageGame));  // …and nav still refuses
  GameConfig onlyStage{};
  onlyStage.stageGame = 0x8010637Cu;                            // stage PC known, table not
  CHECK_EQ(task0_stage_entry_addr(&onlyStage), 0u);
  CHECK(!(task0_stage_entry_addr(&onlyStage) && onlyStage.stageGame));
  // MODE=skip must refuse in the second case too — its gate word is unreachable without the table.
  CHECK(!task0_state_mach_addr(&onlyStage));
  // And the FULL pair is the only combination that arms, so this test discriminates rather than
  // just answering "false" to everything it is asked.
  const GameConfig full = tomba2_cfg();
  CHECK(task0_stage_entry_addr(&full) && full.stageGame);
}

int main(void) {
  // ORDER IS LOAD-BEARING — RenderNoiseMask::from() warns once per process per kind. The SILENT cases
  // must run before the LOUD ones, or a pass on silence would only mean the warning was already spent.
  RUN(filled_config_is_silent);
  RUN(filled_config_task_addrs_are_the_literals);
  RUN(partial_config_names_the_missing_parts);
  RUN(zero_config_warns_loudly);
  RUN(second_consumer_of_an_empty_mask_is_not_named);
  RUN(zero_config_task_addrs_are_zero_not_offsets);
  RUN(half_filled_config_does_not_arm_nav);
  return pt_summary();
}

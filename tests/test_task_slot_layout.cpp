// test_task_slot_layout.cpp — the scheduler task-SLOT address arithmetic (runtime/recomp/task_slot_
// layout.h), which three compare harnesses navigate and gate by: dualcore.cpp's REACH_GAME predicate,
// selftest.cpp's stage machine, and sbs.cpp's auto-nav + MODE=skip observable gate + task-slot dump.
//
// WHY IT IS WORTH A TEST. These addresses used to be Tomba!2 literals copied into each of those files
// (TASKBASE 0x801fe000, TASK0_ENTRY 0x801fe00c, TASK0_BASE+0x4a/0x4e, and 0x801FE000/0x70/3 in sbs's
// dump). Two failure directions, both of which read as an answer:
//   * WRONG ARITHMETIC on the game that HAS the fields — the derived address must come out EQUAL to the
//     literal it replaced, byte for byte, or every harness silently navigates by the wrong word.
//   * ZERO on a game that has NOT RE'd its task table (spyro, spider1 — honest zeros with a TODO). Then
//     the address must be 0 and the CALLER must refuse, because reading guest address 0x0C SUCCEEDS: it
//     answers with BIOS/zero memory. `mem_r32(0x0C) == stageGame(0)` is TRUE on frame 0, so a harness
//     that treats 0 as an address declares "GAME @f0" during the BIOS boot and compares boot noise as
//     gameplay — a false clean bill of health, which is worse than not running.
//
// Hermetic: GameConfig is a POD and these are pure functions over it. No Core, no GPU, no disc.
#include "testutil.h"
#include "task_slot_layout.h"

// Tomba!2's real values (Tomba2Engine/game/core/game_config.cpp:72-74) — the only game that has RE'd
// its task table today.
static GameConfig tomba2_cfg() {
  GameConfig c{};
  c.taskTableBase  = 0x801FE000u;
  c.taskSlotStride = 0x70u;
  c.taskCount      = 3u;
  return c;
}

// EVERY derived address must reproduce the literal it replaced, exactly.
static void test_addresses_reproduce_the_literals(void) {
  const GameConfig cfg = tomba2_cfg();
  CHECK_EQ(task0_stage_entry_addr(&cfg),    0x801FE00Cu);   // dualcore/sbs TASK0_ENTRY, selftest stage()
  CHECK_EQ(task0_state_mach_addr(&cfg),     0x801FE048u);   // sbs vabBuildPending, selftest sm48()
  CHECK_EQ(task0_field_submode_addr(&cfg),  0x801FE04Au);   // sbs SM_S4A
  CHECK_EQ(task0_field_runstate_addr(&cfg), 0x801FE04Eu);   // sbs SM_S4E
  // sbs.cpp's task-slot dump walked base + slot*0x70 for slot 0..2.
  CHECK_EQ(task_slot_base(&cfg, 0), 0x801FE000u);
  CHECK_EQ(task_slot_base(&cfg, 1), 0x801FE070u);
  CHECK_EQ(task_slot_base(&cfg, 2), 0x801FE0E0u);
}

// The table's EXTENT, which is what addrLabel()'s "task_slots" window is derived from. The literal it
// replaced was 0x801FE000..0x801FE200 — 0xB0 bytes too wide: 3 x 0x70 ends at 0x801FE150, and
// 0x801FE150.. is task-0's STACK (sbs.cpp's own pc_skip note says so, and masks it separately). So the
// old label called stack scratch "task_slots".
static void test_table_extent_is_base_plus_count_times_stride(void) {
  const GameConfig cfg = tomba2_cfg();
  const uint32_t end = cfg.taskTableBase + cfg.taskCount * cfg.taskSlotStride;
  CHECK_EQ(end, 0x801FE150u);
  CHECK(0x801FE14Fu <  end);      // last byte of slot 2 — inside
  CHECK(!(0x801FE150u < end));    // first byte of task-0's stack — NOT a task slot
  CHECK(!(0x801FE1FFu < end));    // …nor is anything up to the old literal's 0x801FE200 bound
  int scanned = 0, inside = 0;    // the denominator, so "it fits" is not a vibe
  for (uint32_t a = cfg.taskTableBase; a < 0x801FE200u; a++) { scanned++; if (a < end) inside++; }
  CHECK_EQ(scanned, 0x200);
  CHECK_EQ(inside,  0x150);       // the old label over-claimed 0xB0 bytes
}

// THE HONEST ZERO. A game that has not RE'd its task table gets 0 from every accessor — never a
// plausible-looking low address that mem_r32 would happily answer.
static void test_unset_config_yields_zero_not_an_address(void) {
  GameConfig zero{};
  CHECK_EQ(task0_stage_entry_addr(&zero),    0u);   // NOT 0x0000000C
  CHECK_EQ(task0_state_mach_addr(&zero),     0u);   // NOT 0x00000048
  CHECK_EQ(task0_field_submode_addr(&zero),  0u);
  CHECK_EQ(task0_field_runstate_addr(&zero), 0u);
  CHECK_EQ(task_slot_base(&zero, 0),         0u);
  CHECK_EQ(task_slot_base(&zero, 2),         0u);   // not 0x00000000 + 2*0 either way — still 0
  // A null config is the same case, not a crash — the harnesses call these before a Game exists.
  CHECK_EQ(task0_stage_entry_addr(nullptr),  0u);
  CHECK_EQ(task0_state_mach_addr(nullptr),   0u);
  CHECK_EQ(task_slot_base(nullptr, 1),       0u);
  // And the DERIVED PREDICATE the harnesses gate on must be false, so they take the refuse path. This
  // is the assertion that pins the actual bug: with stageGame 0 and entry 0, `mem_r32(entry) ==
  // stageGame` would be 0 == 0 — TRUE — on frame 0.
  const bool navKnown = task0_stage_entry_addr(&zero) && zero.stageGame;
  CHECK(!navKnown);
  const uint32_t nSlots = zero.taskTableBase ? zero.taskCount : 0u;
  CHECK_EQ(nSlots, 0u);            // the slot dump prints its refusal line and dumps nothing
}

// A PARTIAL config: a game that filled the base but not the count still gets valid addresses for task
// 0 (which is what the nav predicate needs) while the slot WALK is empty rather than guessed at 3.
static void test_partial_config_gives_task0_but_no_walk(void) {
  GameConfig cfg{};
  cfg.taskTableBase = 0x80190000u; cfg.taskSlotStride = 0x40u;   // count deliberately left 0
  CHECK_EQ(task0_stage_entry_addr(&cfg), 0x8019000Cu);
  CHECK_EQ(task0_state_mach_addr(&cfg),  0x80190048u);
  CHECK_EQ(task_slot_base(&cfg, 1),      0x80190040u);           // stride is the game's, not 0x70
  const uint32_t nSlots = cfg.taskTableBase ? cfg.taskCount : 0u;
  CHECK_EQ(nSlots, 0u);
  const uint32_t end = cfg.taskTableBase + cfg.taskCount * cfg.taskSlotStride;
  CHECK_EQ(end, cfg.taskTableBase);    // an EMPTY window: labels nothing, rather than 3 invented slots
}

int main(void) {
  RUN(addresses_reproduce_the_literals);
  RUN(table_extent_is_base_plus_count_times_stride);
  RUN(unset_config_yields_zero_not_an_address);
  RUN(partial_config_gives_task0_but_no_walk);
  return pt_summary();
}

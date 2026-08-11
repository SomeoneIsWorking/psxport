// task_slot_layout.h — STOPGAP, and the ONLY place a scheduler task-SLOT FIELD OFFSET may be written.
//
// The compare harnesses (dualcore.cpp, sbs.cpp, selftest.cpp) all ask the same two questions of the
// guest scheduler: "which stage body is task 0 running" (slot + 0x0C) and "where is that stage's state
// machine" (slot + 0x48). The BASE of the table is game data and comes from
// GameConfig::taskTableBase/taskSlotStride/taskCount. THE OFFSETS WITHIN A SLOT ARE ALSO GAME DATA —
// they are Tomba!2's task-object layout — and GameConfig has no field for them yet.
//
// STOPGAP: add GameConfig::taskStageEntryOff (+ a state-machine offset) and delete this header, because
// a game with a different task object silently reads the wrong words today. Until then these live in
// exactly ONE place: they were previously copied into three files, which is how the packet-pool literal
// managed to be fixed in one file and stay wrong in two.
//
// Consumers must still treat an UNSET taskTableBase as fatal for the thing they were about to measure:
// task0_stage_entry_addr() returns 0 for it, and 0 is not an address you may read — mem_r32(0x0C)
// answers with BIOS/zero memory and every equality test against it becomes a lie, not a no-op.
#pragma once
#include <stdint.h>
#include "game_iface.h"

constexpr uint32_t kTaskSlotStageEntryOff = 0x0Cu;   // slot + 0x0C = PC of the stage body running there
constexpr uint32_t kTaskSlotStateMachOff  = 0x48u;   // slot + 0x48 = that stage's state-machine word

// Guest address of task 0's stage-entry word, or 0 when the game has not RE'd its task table.
inline uint32_t task0_stage_entry_addr(const GameConfig* cfg) {
  return (cfg && cfg->taskTableBase) ? cfg->taskTableBase + kTaskSlotStageEntryOff : 0;
}
// Guest address of task 0's stage state-machine word, or 0 when un-RE'd.
inline uint32_t task0_state_mach_addr(const GameConfig* cfg) {
  return (cfg && cfg->taskTableBase) ? cfg->taskTableBase + kTaskSlotStateMachOff : 0;
}

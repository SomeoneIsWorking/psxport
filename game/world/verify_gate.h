// game/world/verify_gate.h — shared A/B verify-gate helper for the WORLD OBJECT subsystem.
// record_gate runs a native object-subsystem fn, snapshots+rolls back, super-calls the recomp body, and
// diffs full main-RAM (excluding the dead stack window) + scratchpad + v0. Shared by spawn.cpp and
// graphics_bind.cpp. These gates are DORMANT diagnostic channels (REPL `debug <chan>`); off by default.
#ifndef GAME_WORLD_VERIFY_GATE_H
#define GAME_WORLD_VERIFY_GATE_H
struct Core;
void rec_super_call(Core*, uint32_t);
void record_gate(Core* c, uint32_t (*fn)(Core*), uint32_t super_addr, const char* gate, int on);
#endif

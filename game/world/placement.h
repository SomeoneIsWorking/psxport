// class Placement — PC-native field OBJECT-PLACEMENT subsystem.
//
// PROPER OOP: one instance per Core, embedded as `Core::engine::placement`. Back-pointer wired
// once by Core's constructor. Callers reach the placement driver through the object graph:
//
//     eng(c).placement.placeAreaObjects();     // FUN_80072A78 — field object-placement driver
//     eng(c).placement.spawnWithParent();      // FUN_80072DDC — single-object helper
//                                                 //   (args in c->r[4..7] like the guest)
//
// The placement driver reads the active area's placement TABLE and populates the field with its
// NPCs/items/scenery via the owned spawn dispatcher. Underlying guest bodies stay as reference /
// super-call for the byte-A/B verify gates.
#ifndef GAME_WORLD_PLACEMENT_CLASS_H
#define GAME_WORLD_PLACEMENT_CLASS_H
class Core;
class Placement {
public:
  Core* core = nullptr;
  void placeAreaObjects();   // FUN_80072A78
  void spawnWithParent();    // FUN_80072DDC (args in c->r[4..7])
};
#endif

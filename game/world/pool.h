// game/world/pool.h — PC-native object-pool / control-block init (WORLD subsystem) public entries.
#ifndef GAME_WORLD_POOL_H
#define GAME_WORLD_POOL_H
struct Core;
// 0x8007B18C — top-level object-pool init (field case-0 prefix). GATED (channel `poolinitverify`).
void ov_pool_init_run(Core* c);
// 0x800796DC — control-block reset + sub-inits (field case-0 prefix). GATED (channel `init796dcverify`).
void ov_796dc_run(Core* c);
// 0x800263E8 — area object-record seeding (field case-0 prefix). GATED (channel `init263e8verify`).
void ov_263e8_run(Core* c);
// 0x80075240 — clamp/control-block reset (field case-0 prefix). GATED (channel `init75240verify`).
void ov_75240_run(Core* c);
// 0x800783DC — per-area view/scroll setup (field case-0 prefix). GATED (channel `init783dcverify`).
void ov_783dc_run(Core* c);
// 0x80078610 — final per-area view init (field case-0 prefix). GATED (channel `init78610verify`).
void ov_78610_run(Core* c);
#endif

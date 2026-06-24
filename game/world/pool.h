// game/world/pool.h — PC-native object-pool / control-block init (WORLD subsystem) public entries.
#ifndef GAME_WORLD_POOL_H
#define GAME_WORLD_POOL_H
struct Core;
// 0x8007B18C — top-level object-pool init (field case-0 prefix). GATED (channel `poolinitverify`).
void ov_pool_init_run(Core* c);
#endif

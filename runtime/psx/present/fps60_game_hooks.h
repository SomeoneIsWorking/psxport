// fps60_game_hooks.h — guarded game callbacks owned exclusively by temporal presentation.
#pragma once

class Core;
struct GameHooks;

// Read a game-owned scene view matrix for a native projection path. An absent reader is not a
// usable result: false tells Fps60 to refuse rather than inventing a camera.
bool game_fps60_read_scene_cam(Core *c, const GameHooks *hooks, float R[3][3], float T[3]);

// Re-run the game-owned native world producer for an interpolated present. Absence is not an empty
// world: false tells Fps60 to refuse the tier rather than discard captured geometry.
bool game_fps60_world_pass(Core *c, const GameHooks *hooks, float t);

// Rotate game-owned temporal history after the two presents. Games without either history stream
// have nothing to rotate, so absence is a genuine no-op.
void game_fps60_bb_swap_prev(Core *c, const GameHooks *hooks);
void game_fps60_temporal_rotate(Core *c, const GameHooks *hooks);

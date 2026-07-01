// engine/cull.h — PC-native visibility CULL / LOD subsystem.
// The engine OWNS the per-object visibility decision (per CLAUDE.md: render ordering / visibility is the
// engine's, with its own widescreen-aware margin). The per-object cull body (FUN_8007712C), its two
// camera-relative wrappers (FUN_8007778C / FUN_80077ACC), and the standalone view-cone cull
// (FUN_8002B278). Extracted from game_tomba2.cpp into its own module (PC-game code structure);
// registered in game_tomba2.cpp's init block by these names.
#ifndef ENGINE_CULL_H
#define ENGINE_CULL_H
struct Core;
void ov_object_cull(Core* c);       // FUN_8007712C — per-object cull body (+ widescreen margin re-include)
void ov_cull_wrapper(Core* c);      // FUN_8007778C — camera-relative delta + flag reset → cull body
void ov_cull_wrap_77acc(Core* c);   // FUN_80077ACC — cull-wrapper variant (flags 1/4, caller-supplied pos)
void ov_cone_cull_2b278(Core* c);   // FUN_8002B278 — standalone view-cone cull
#endif

// Tomba2Engine — PC-native engine init stub. The per-object BEHAVIOR-HANDLER dispatcher (formerly
// hosted here as free functions dispatch_obj_method / dispatch_native_behavior / behavior_native_name
// + the 50-entry NativeBeh table) is now `class BehaviorDispatch` on Engine — see
// game/object/behavior_dispatch.{h,cpp}. Callers reach it as `c->engine.behaviors.method(...)`.
//
// The per-frame OBJECT-LIST walk (guest FUN_8007A904) lives in `class ObjectList` on Engine
// (game/object/object_list.cpp); the entity_walk_7a904 free function that used to live here has
// moved. This file keeps only the boot-time init hook.
#include "cfg.h"
#include <stdio.h>

void engine_tomba2_init(void) {
  if (cfg_dbg("engine"))
    fprintf(stderr, "[engine] native object-list walk active (FUN_8007a904)\n");
}

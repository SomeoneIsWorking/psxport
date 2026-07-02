// engine/hud.h — PC-native in-game HUD drawer (engine/hud.cpp).
// Post-override-removal (2026-06-22) this header is a placeholder — the HUD draw entry points in
// hud.cpp are all file-local (anonymous namespace) and unreached, awaiting direct-call wiring from
// a native parent. Kept so #include "hud.h" stays a valid marker in files that intend to use the
// HUD subsystem later.
#pragma once

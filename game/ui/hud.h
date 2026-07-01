// engine/hud.h — PC-native in-game HUD drawer (engine/hud.cpp).
// Registers native overrides for the HUD sprite-strip / rect draw helpers so the in-game HUD (the
// spiky-ball weapon indicator, AP/heart gauge cells, UI panel slices) is drawn by the PC renderer's
// 2D overlay layer instead of the PSX GP0 packet emitter. Call once from the game init.
#pragma once

void hud_register(void);

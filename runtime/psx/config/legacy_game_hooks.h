// legacy_game_hooks.h — deprecated callback bag for consumers migrating to GameRuntime overrides.
#pragma once

#include "legacy_game_config.h"

class Core;
class Game;

// Framework-side POD mirror of ScreenFade::State, read without naming a game type.
struct FadeState {
  int mode;
  unsigned char r, g, b;
};

struct GameHooks {
  void *(*ctxCreate)(Core *c);
  void (*ctxDestroy)(void *ctx);

  void (*frameUpdate)(Core *c);
  void (*drawOTag)(Core *c, uint32_t otHead);

  void (*audioMixFrame)(Core *c, int16_t *buf, int frames);
  const char *(*audioNowPlayingName)(Core *c);
  void (*audioSoundTestPlay)(Core *c, int track);
  void (*bootInit)(Core *c);
  void (*registerOverrides)(Game *g);

  void (*renderFadeState)(Core *c, FadeState *out);
  void (*renderBbFrameReset)(Core *c);

  bool (*replCommand)(Core *c, const char *cmd, const char *line);
  int (*devAreaCount)(Core *c);
  const char *(*devAreaName)(Core *c, int area);
  bool (*devWarpAllowed)(Core *c);

  // Transitional until games submit immutable recipes that the framework interpolates directly.
  void (*fps60WorldPass)(Core *c, float t);
  void (*fps60BbSwapPrev)(Core *c);
  void (*fps60TemporalRotate)(Core *c);

  int (*selftestGame)(const char *which, const char *exePath);

  // Null means this game has no native-camera path; Fps60::sceneCam refuses that request.
  void (*fps60ReadSceneCam)(Core *c, float rotation[3][3], float translation[3]);
};

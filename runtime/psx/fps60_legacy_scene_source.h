// Explicit adapter for consumers using GameHooks and Fps60's camera/object capture chokes.
#pragma once

#include "temporal_scene_source.h"
#include <memory>

class Game;
std::unique_ptr<TemporalSceneSource> makeLegacyTemporalSceneSource(Game &game);

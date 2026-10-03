// Explicit adapter for consumers using GameHooks and Fps60's camera/object capture chokes.
#pragma once

#include "in_between_strategy.h"
#include <memory>

class Game;
std::unique_ptr<InBetweenStrategy> makeHostWorldPassStrategy(Game &game);

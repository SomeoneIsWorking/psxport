// active_low_edges.h — deterministic edge detection for PSX active-low button masks.
//
// This class knows only electrical/button semantics: a 1->0 transition is a press and a 0->1
// transition is a release. It deliberately does not know what Start means in any game state. Logo,
// loading-overlay, movie, and scripted-sequence transitions remain owned by the game or player that
// can prove the correct destination state; the framework must never swallow a frame globally.
#pragma once
#include <cstdint>

class ActiveLowEdges {
public:
  void reset(uint16_t current = 0xFFFFu) {
    mPrevious = current;
    mPressed = 0;
    mReleased = 0;
  }

  void sample(uint16_t current) {
    mPressed = (uint16_t)(mPrevious & (uint16_t)~current);
    mReleased = (uint16_t)(current & (uint16_t)~mPrevious);
    mPrevious = current;
  }

  uint16_t pressed() const { return mPressed; }
  uint16_t released() const { return mReleased; }
  bool pressed(uint16_t mask) const { return (mPressed & mask) != 0; }
  bool released(uint16_t mask) const { return (mReleased & mask) != 0; }

private:
  uint16_t mPrevious = 0xFFFFu;
  uint16_t mPressed = 0;
  uint16_t mReleased = 0;
};

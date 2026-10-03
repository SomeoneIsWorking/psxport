#pragma once

#include <cstdint>

struct RqItem;
class GpuState;

// "I can see a coloured thing on screen — WHO DREW IT?" answered by colour instead of by position.
//
// The pixel probe (PSXPORT_PRIMAT) answers the same question by coordinate, but it can only report
// prims whose triangles cover one exact pixel: a two-pixel artefact is missed by a probe aimed two
// pixels away, and the probe then reports the large prims that happen to share the spot, which reads
// like a confident answer about the wrong prim (measured on Spyro's Artisans courtyard, 2026-09-19,
// where probing a red dot returned a tan hedge quad). Colour is the property the human observer
// actually has, so this census matches on it and reports every prim in the frame that carries it.
//
// The negative is the point of the instrument: it always reports how many prims it scanned, so
// "matched 0" is distinguishable from "never ran".
//
// Knob: PSXPORT_PRIMRGB="R,G,B[,frame[,tolerance]]" — 0-255 channels, an optional first frame to
// start at, and an optional per-channel tolerance (default 24). A malformed value is refused, not
// silently reinterpreted.
class PrimColorCensus {
public:
  // Called for every prim as the render queue flushes it. Detects the frame boundary itself and
  // reports the previous frame's totals, so no caller has to remember to close a frame.
  void observe(GpuState &gpu, const RqItem &item);

private:
  bool configure();
  int matchingVertex(const RqItem &item) const;
  void reportFrame() const;

  bool mConfigured = false;
  bool mEnabled = false;
  int mRed = 0;
  int mGreen = 0;
  int mBlue = 0;
  int mFromFrame = 0;
  int mTolerance = 24;
  int mFrame = -1;
  long mScanned = 0;
  long mMatched = 0;
};

// snapshot.h — capture guest RAM from a RUNNING port, not only from a dying one.
//
// WHY. The guest-exec-MISS dump (hle.cpp) proved that a 2 MB image of guest RAM is the fastest way to
// answer almost any question about port state — it is what identified the resident overlay that a
// whole diagnosis had been built on the wrong image of. But it only fires when the port ABORTS, so
// every question about a HEALTHY run ("what is in the OT pointer right now", "which overlay is live
// at the title screen") required either crashing the port or adding a bespoke probe and rebuilding.
//
// This makes the same capability available on demand:
//   PSXPORT_SNAP_AT=120,900,3000   snapshot at these frame numbers
//   PSXPORT_SNAP_EVERY=600         snapshot every N frames (bounded by PSXPORT_SNAP_MAX, default 8)
//   kill -USR1 <pid>               snapshot at the next frame boundary
//
// Each snapshot writes scratch/raw/snap_<frame>.bin plus a .txt sidecar naming what it is, so a
// directory of dumps is self-describing rather than a pile of anonymous 2 MB files. Import a .bin at
// 0x80000000 in the maintained Ghidra workflow or point a title-owned memory query tool at it.
//
// The frame boundary is the game's, not ours: a port whose guest still owns its frame loop calls
// snapshot_tick() from wherever it knows a frame ended (for Spyro that is the vblank wait). Capturing
// mid-frame would catch the OT and packet pool half-built, which is precisely the state nobody wants
// to reason about.
#pragma once
#include <cstdint>
#include <vector>
class Core;

namespace psx::debug {

// One per Game: the scheduled and SIGUSR1-requested guest RAM captures.
class Snapshotter {
public:
  // Call once per completed frame. Cheap when nothing is scheduled; writes at most PSXPORT_SNAP_MAX images per run.
  void tick(Core &core);
  // Capture now, tagged with a reason that lands in the sidecar. True if written.
  bool captureNow(Core &core, const char *why);

private:
  void configure();

  bool configured_ = false;
  std::vector<uint64_t> at_; // explicit frame numbers
  uint64_t every_ = 0;       // periodic interval, 0 = off
  int max_ = 8;              // hard cap on images per run
  int written_ = 0;
  uint64_t frame_ = 0;
};

} // namespace psx::debug

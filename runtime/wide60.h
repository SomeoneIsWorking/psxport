// wide60: object/transform-based 60fps interpolation via a custom reprojecting
// renderer. Captures, per frame, the projected geometry (GP0 polygons) joined
// to the GTE transform that produced each vertex (so vertices carry their local
// coordinates + object transform). Matched across logic frames by transform
// identity, lerped, reprojected with a faithful RTPS, and rasterized to a custom
// framebuffer. See patches/tomba2/objects.md for the RE behind it.
#pragma once

#include <cstdint>

// Register the GTE/RTP/GPU capture hooks. Call once after game load.
void Wide60_Install();

// Called once per host frame (after retro_run). Finalizes the frame's capture;
// when verbose, logs join coverage / counts.
void Wide60_FrameEnd(unsigned frame);

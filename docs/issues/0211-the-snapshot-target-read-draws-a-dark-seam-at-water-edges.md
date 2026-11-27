# 0211 — The snapshot target read draws a dark seam at water edges

Status: open (found 2026-11-27). Owner: the record rasterizer's snapshot path (`RecordRasterizer`, the planner's batch
close in `record_raster_setup.cpp` `Builder::commit`); only devices without an in-pass read take it (D3D12 today).

## Observed
Tomba! 2, the 2026-11-20 village culling report's `state.bin`, presents 4-20, ires 8x, 3840x2160. With the target
read from a snapshot (system SDL, before the in-pass read), a one-native-pixel dark line runs along the bottom-right
water edge, about 0.01% of the frame, the same in two runs. The in-pass read on RADV (barrier) and on llvmpipe
(raster order) both draw no line, and llvmpipe sides with the in-pass picture at every pixel where the two differ.

## Next
Not reachable on Vulkan or Metal any more. Reproduce on a device without the in-pass read and trace which draw reads a
stale snapshot pixel there.

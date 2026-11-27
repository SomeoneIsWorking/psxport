# 0210 — Vulkan validation reports a fence reset while it is in use

Status: open (found 2026-11-27). Owner: not yet traced; SDL_GPU's Vulkan backend (`vkResetFences` in the fence pool) or
`RenderDevice`'s submit and acquire order.

## Observed
Tomba! 2, the 2026-11-20 village culling report's `state.bin`, `PSXPORT_GPU_DEBUG=1`, RX 6700 XT (RADV), 40 frames:
`VUID-vkResetFences-pFences-01123` (a fence reset while queue work that signals it is still pending), 20 times,
until the layer's duplicate limit. Both the system SDL 3 and the pinned fork (4a641027) show it, so it is older
than the fork's in-pass read.

## Next
Trace which fence: SDL's pooled fence for a submitted command buffer, or one psxport waits on (`acquire`,
`timeSubmits_`).

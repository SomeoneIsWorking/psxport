# Tomba! 2 (SCUS-94454) — object cull-cone widening

## Problem
The engine culls world objects (NPCs etc.) against a view cone that is slightly
narrower than the visible screen: objects near the screen edge despawn while
still (partially) visible — noticeable in 4:3, and worse with the GTE widescreen
hack since the visible FOV grows but the cull cone does not (user repro: the
fisherman near the left edge of the hill screen is culled until you step left).

## Mechanism (RE'd 2026-06-12 from a live RAM dump, savestate on the hill screen)
Object enqueue-for-draw function (overlay code, ~0x80077100-0x800776E0): for
each object it computes a view-cone test

    v1 = dot(camera_row, object_direction) / (distance-ish >> 10)   ; cos-scaled
    slti $v0, $v1, THRESHOLD
    bnez $v0 -> skip (cull)        ; v1 < THRESHOLD = outside cone = not drawn
    ... else: sb 1, 1(obj) (visible flag) + enqueue to scratchpad draw queue

Six `slti $v0, $v1, imm` sites with thresholds 0x370/0x358/0x358/0x370/0x350/0x368
(three near/mid/far variants x two paths). Lower threshold = wider cull cone.

## Patch
Scale thresholds by ~0.72 (covers 16:9's 4/3 wider FOV plus margin). Verified:
at the repro savestate the engine draws 13 objects instead of 5, the edge NPCs
(fisherman) included; walking pop-ins drop from 12 to 5 in a 260-frame run.

RAM pokes (overlay code -> conditional "old:new" form; only applied while the
original word is present, so other overlays at the same addresses are safe):

    PSXPORT_POKE="800772D4=28620370:28620278;80077368=28620358:28620268;80077414=28620358:28620268;800774A8=28620370:28620278;8007753C=28620350:28620260;800775D0=28620368:28620270"

Each entry: `slti $v0,$v1,OLD` -> `slti $v0,$v1,NEW` (same instruction, smaller
immediate): 0x370->0x278, 0x358->0x268, 0x350->0x260, 0x368->0x270.

Tuning: immediates scale linearly; raise them toward the originals if distant
objects pop in too eagerly, lower further for ultrawide.

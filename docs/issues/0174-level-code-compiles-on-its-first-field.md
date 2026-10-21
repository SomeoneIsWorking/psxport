# 0174: level code compiles on its first field

Status: closed, no visible defect. Owner: `runtime/cpu/lightrec_executor.cpp` `LightrecExecutor::executeWithBoundary`.

## Observed
psx-spyro issue 0196, Spyro 1 `warp 10` from the title at real speed, fps60=1, 8x on the 4K sink. The load field
(`fields=0/1`: fence 4087 in pass 4 `final`, 4083 in `review2`, 4082 in `fprof`, 4085 in pass 5 `final5`) comes
51-72 ms after the previous present: the field's own work overran its interval and `pacePlan` dropped the debt. On
a simulated 60.00 Hz display the picture before the load field is held for 3-4 vblanks. Before pass 5 the deadline
resynced to the moment the field ended, off the vblank grid, so the first in-between after it was 11-16 ms late and
later presents 1.9-4.4 ms late judder too; since pass 5 whole intervals are dropped (`final5`: due step 66.67 ms,
hold 4 vblanks, nothing late after it).

What is held (`scratch/judder-pass5/probe-arrival`, 1x presents from the warp on): the "THE ADVENTURE
BEGINS..." card, text on black (mean luma 1.8-1.95 of 255, max 244), unchanged but for a slow fade over the 13
fields before the load. The load field shows the level's first near-black fade-in picture (mean luma 0.29), and the
late in-between of the earlier runs fell in that fade (0.7-1.9). Retail shows that card for the whole CD load, so holding it 50-70 ms
longer moves nothing on screen.

Spyro 2 and 3 (psx-spyro 0196 pass 6): the load field of `warp 21` (fence 1982, `judge-s2`) is presented 42.67 ms
after its due time and `warp 10` on Spyro 3 (fence 2543, `judge-s3`) 58.65 ms after; the simulated display holds the
picture before each 2-3 vblanks longer. Whether that picture moves was not checked.

## Cause
`lightrec_execute`, called from `executeWithBoundary`, compiles each cold block synchronously on the guest thread.
The WAD load invalidates the level image, so the load field runs the level's code cold and compiles it. Perf
sampling the main thread at 2 kHz (pass 4 `fprof`) puts Lightrec compile symbols (`_jit_*`, `_emit_code`) in 75 of
the 131 samples of the load field, and in 30 of 231 over the next four fences against 9 of 195 in steady play.

## Why closed
The stall is real cost but it lands on a still card, so the player sees no judder. Compiling the image's blocks at
the load commit would not remove it: on a first residency nothing is known to compile, and on a later one it moves
the same work into the same field. Taking the compile off the presentation path needs either translation ahead of
the load field, during the load's own guest stages (Spyro's archive transfer is instant, so there are none to hide
it in), or a threaded compiler in the Lightrec fork with the guest waiting on it, which shortens nothing.
Interpreting first and compiling later is not allowed (product execution contract). Reopen if a title holds a
moving picture across a load field.

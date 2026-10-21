# 0174: level code compiles on its first field

Status: open. Owner: `runtime/cpu/lightrec_executor.cpp` `LightrecExecutor::executeWithBoundary`.

## Observed
psx-spyro issue 0196 pass 4, Spyro 1 `warp 10` at real speed with fps60=1 on the 4K sink. The load field (fence
4082, `fields=0/1`) is presented on time. The first in-between after it (fence 4083, `cut=1`) is presented 16 ms
after its due time, and the presents up to fence 4086 are 1-10 ms late. On a simulated 60.00 Hz display that is
one present held for 4 vblanks.

## Cause
`lightrec_execute`, called from `executeWithBoundary`, compiles each cold block synchronously on the guest thread.
The WAD load invalidates the level image, so the logic of the first fields after arrival recompiles the level's
code. An in-between needs the next field's logic, so it cannot be built until that compile has finished. Perf
sampling the main thread at 2 kHz finds Lightrec compile symbols (`_jit_*`, `_emit_code`) in 30 of the 231 samples
from fence 4082 to 4086, against 9 of 195 in a steady window of the same length, and in 75 of 131 samples during
the load field. Snapshot misses (`HostMemory::fallThrough`) add another 13 samples against 4.

## Proper fix
Translate the code of a newly loaded image before its first field: at the load commit that invalidates it,
compile the blocks the image's previous residency reached (the function reach already knows them), so the compile
cost falls inside the load the player already sees. The pinned Lightrec (`57fd2070`) has no threaded compiler, so
compiling off the guest thread would first need a fork commit. Interpreting first and compiling later is not
allowed (product execution contract).

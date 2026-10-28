# 0194: steady-state CPU work the player never sees

Status: fixed (2026-10-28) for the costs below (VramBitmap, FMV AC decode, block-boundary dispatch); the rest is design, listed under Left. Owner:
`runtime/psx/gpu/vram_bitmap.h` `VramBitmap`, `runtime/psx/movie/fmv_decode.cpp`.

## Observed
Steady state per fence from boot, unpaced, ires 0, fps60 1, sink 3840x2160, hidden window, Clang: the run to fence 2400
minus the run to 1200, over 1200 (`scratch/cost-audit/`, `cost.py`). Instructions and task clock are `perf stat`
user counts of the whole process; allocations count every `malloc`/`new` (guest thread in brackets). Instruction
and allocation counts are load-independent; task clock is not. The window is wherever boot is by then (Toy Story 2
and Tomba! 2 are in their intro movies) and moves between runs. Numbers are after the fixes.

| title | presents/fence | Minstr/fence | Minstr/present | task ms/fence | allocs/fence (guest thread) |
|---|---|---|---|---|---|
| spyro | 0.52 | 126.8 | 244.6 | 9.72 | 73.0 (72.5) |
| toystory2 | 1.13 | 19.9 | 17.6 | 3.50 | 29.1 (28.2) |
| crashbash | 1.88 | 120.7 | 64.2 | 13.33 | 378.1 (376.6) |
| crash | 1.83 | 79.1 | 43.1 | 5.68 | 227.2 (225.9) |
| ctr | 1.07 | 37.4 | 34.9 | 2.79 | 84.5 (84.1) |
| tomba2 | 1.99 | 23.9 | 12.0 | 3.67 | 56.8 (55.0) |
| spiderman | 0.84 | 15.3 | 18.3 | 1.39 | 36.0 (35.5) |
| vagrant | 1.08 | 59.1 | 54.8 | 4.79 | 29.3 (28.2) |
| megamanx4 | 0.59 | 13.2 | 22.6 | 1.42 | 29.3 (23.2) |
| tekken3 | 0.42 | 31.6 | 75.6 | 3.18 | 37.2 (36.9) |
| c12 | 1.00 | 169.3 | 169.9 | 13.22 | 31.4 (32.3) |

## Cause
- `VramBitmap::clear` memset its 64 KB on every texture-page change: 2.4% of Crash 1's instructions.
- `fmv_decode.cpp` decoded each AC code by a linear search over 16 lengths × 111 codes; `decodeBsFrame` was 38% of
  the Toy Story 2 and Tomba! 2 boot windows.

## Fix
- `VramBitmap` tracks the rows `mark` set and `clear` touches only those; `fillAll` marks every row. Crash 1:
  `VramBitmap::clear` 0.41%.
- AC codes decode through two-level constexpr tables. 150 frames of `ACTI.STR` through `fmv_export`: 7.34G to
  6.43G instructions; Toy Story 2 37.5 to 19.9 and Tomba! 2 37.4 to 23.9 Minstr per fence.

## Proof
Review: the AC table equals the old linear search for all 65536 16-bit windows (`scratch/cost-audit-review/ac_check.cpp`,
0 mismatches), and `clear` equals the per-row clear because `mark` is the bitmap's only writer. The byte-identical
frame and audio comparisons made during the pass kept no hashes and their dumps were swept, so they are not evidence.

## Block boundary dispatch (2026-10-28)
Cause: `NativeDispatcher::classify` rebuilt its epoch from `Core::imageCatalog()`, `ImageCatalog::revision()`, its
own revision and the platform-HLE revision on every block entry, called the out-of-line pad-work-area check before
the cache, and the executor found the boundary context by thread id. Suppressions (`callOriginal`) moved the epoch and
flushed every verdict.
Fix: one `DispatchEpoch` on Core that its input owners advance; the verdict slot stores its epoch and a `live` flag;
the classify fast path is inline; a suppression drops its own address's slot; the boundary context is the pointer set
around `lightrec_execute`.
Proof (`scratch/dispatch-cost/`, `run.py`, `digest.py`; hidden, unpaced, ires 0, fps60 1, sink 3840x2160; Clang;
`perf stat -e instructions:u` of the whole process; Minstr per fence = run to N2 minus run to N1, over N2 - N1):

| screen | before | after |
|---|---|---|
| Tekken 3 fight, `warp 0 1` at present 300, fences 2500 to 4500 | 80.33, 80.34 (two runs) | 77.90 (-3.0%) |
| Vagrant Story save point 0 free roam (New Game route, `warp 0` at 14000), fences 16000 to 18000 | 26.09 | 25.01 (-4.1%) |

Behaviour unchanged: the field digests (emulated ticks and main-RAM hash per field) of a boot run are identical
before and after, Tekken 3 3000 fields and Vagrant Story 6000 fields, and so are the present-shot hashes at fences
500/1000/2000/3000 and 1000/2000/4000/6000; the fight's present at 2500 and 4500 and the free roam's at 16000 and
18000 hash the same before and after.

## Left (design, recorded in the owners' docs where one exists)
- Beetle's software raster of device VRAM (`presentation.md` Render paths): about 24% of Crash 1.
- The `RecordPlanner` 1x walk (`presentation.md` Raster): 12-25%; C-12's lines plan as one quad per pixel, about
  half its instructions.
- The owner ledger booking every guest store (`presentation.md` Owner ledger): 2-6%, plus about 3% for
  executable-write notification.
- Block boundary classification: the planned step is done (2026-10-28, below); what is left is the callback itself,
  about 22% of Tekken 3's fight, whose removal needs block linking in the Lightrec fork (`port-framework.md` Dispatch).
- SPU emulation 2-7% and CHD hunk decoding on its worker threads 2-3%; neither has a subsystem doc.
- Allocations: issue 0175 (guest execution) and psx-crash-bash 0046 (its producers).
- SDL3 GPU has no timestamp queries, so there is no GPU time per pass.

// memcensus — WHICH CALL SITES MOVE THE BYTES (`PSXPORT_MEMCENSUS=1`).
//
// WHY THIS EXISTS. The host profile puts __memmove_avx_unaligned_erms at 10-13% of a Tomba!2 frame —
// repeatedly the single largest entry, larger than any function we wrote. A sampled profile can say
// the program counter is inside memmove; it cannot say WHO CALLED IT, and that gap has now produced
// two wrong conclusions on kanban #118:
//
//   * gating the render_geom whole-VRAM snapshot skipped it on 50.0% of calls and changed the frame
//     by nothing;
//   * staging only the dirty rows cut bytes 1024 KiB -> 150 KiB per frame, pixel-identical, and also
//     changed nothing.
//
// Two independent byte cuts, both free, while memmove stayed at the top. Either the copies are not
// the cost, or the copies that matter are somewhere nobody has looked — and `debug vramcopy` cannot
// tell the difference, because it counts only the two copies it was written for. It has no
// denominator over all copies, so its silence about a third source is not evidence.
//
// WHAT THIS DOES INSTEAD. -Wl,--wrap=memcpy / --wrap=memmove routes every call from first-party
// objects through __wrap_memcpy, which attributes the byte count to __builtin_return_address(0) —
// the CALL SITE — before forwarding to the real implementation. Dumped as `addr calls bytes`, which
// tools/prof_hot.py resolves to symbols with the same machinery it uses for PC samples.
//
// WHAT IT CANNOT SEE, stated so a small total is not misread as "nothing else copies":
//   * copies the COMPILER INLINED (a fixed-size struct assignment becomes movs, never a call);
//   * copies made inside a shared object (libc, the Vulkan driver, SDL) — --wrap only rewrites
//     references from objects in THIS link;
//   * anything before memcensus_init() or after the dump. The armed window is reported.
// Those are real blind spots, so the report names them rather than implying a closed accounting.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Arm the census and record the frame it was armed at. No-op unless PSXPORT_MEMCENSUS=1, so an
// ordinary run pays one relaxed load and a not-taken branch per copy.
void memcensus_init(void);

// Write the table to PSXPORT_MEMCENSUS_OUT (default scratch/raw/memcensus.txt). Safe to call twice.
void memcensus_dump(void);

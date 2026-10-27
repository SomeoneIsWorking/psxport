# 0185 — Every guest RAM access takes a Lightrec memory callback

Status: open (2026-10-28). Owner: `runtime/cpu/lightrec_executor.cpp` (`Impl::initializeMaps`, `kMemoryOps`).

## Observed
C-12's boot load (psx-c12 issue 0006) spins in libcd's CdReady wait for drive-paced sectors. Each such field runs the
title's 4M-cycle budget, about 2.0M guest instructions, in 25-40 ms host time. A per-field count on that build gave
about 594k memory callbacks and 273k CPU-store invalidation requests per field (227k of them to the stack page
`0x801FF000`, 45k to `0x80105000`), and Lightrec invalidated 0 blocks.

## Cause
`initializeMaps` gives `PSX_MAP_KERNEL_USER_RAM` the `kMemoryOps` table. In Lightrec's optimizer, a RAM map with ops
turns off direct RAM access: even `$sp`/`$gp` loads and stores are no longer tagged `LIGHTREC_IO_DIRECT` or
`LIGHTREC_NO_INVALIDATE` (`optimizer.c`, `lightrec_flag_io`). Every load and store therefore calls back into
`Core::mem_r*`/`mem_w*`. Every store also reaches `notifyExecutableWrite` and `lightrec_invalidate`, even when the
page holds no code.

## Proper fix
Map RAM direct, so Lightrec owns code-page invalidation. The store observers that now hang off the callback must move
first: `bookStore` (write rollback, packet tracking, `emission.noteStore`), `display_pass_write_guard` and
`storeWatch`. Each needs a seam that does not depend on seeing every store, such as Lightrec's own invalidation hook
for code pages, or page protection for the few watched ranges. This is a framework restructure that changes every
title's store path, so it needs its own pass and an all-title real-time check.

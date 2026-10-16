# 0157: a fill in a pass that drew nothing left the older picture shown

Status: fixed

## Observed
Tomba! 2 real-window play check (2026-10-16, `warp 1`, fps60 on, 16:9): `records: backwards=5`, all at stage
hand-offs (DEMO to GAME at f605 and f607, the warp at f621, the area load end at f686 and f687). Hashing the
displayed VRAM buffer at each record present showed the device black (0 lit pixels) at presents 45, 47 and
61 while the presenter showed records 42 and 58: the title menu picture flashed where the console shows black.

## Cause
`runtime/psx/frame/frame_presenter.cpp:FramePresenter::commitSealed` held only records that drew a primitive,
so `shownRecord` never saw a `VramFill` in an earlier record that drew nothing (Tomba! 2 fills each buffer
black in such passes, then displays it) once that record was no longer the current one, and returned the
older drawing record for a buffer the device had overwritten.

## Resolution
`FramePresenter::holdUndrawnWrites` keeps the written rects of records that drew nothing, latest sequence per
rect, until they are older than the oldest held drawing record; `shownRecord` treats them like a later
drawing record's writes. `gpu::entryWrittenRect` and `gpu::rectsOverlap` are split out of `entryWritesRect`
so both read one rule. Test: `tests/test_record_cadence.cpp`
`a_fill_in_an_earlier_record_that_drew_nothing_makes_the_buffer_stale` (backwards 1 before, 0 after).
Tomba! 2 play check after: `backwards=1`; the one left is the guest's own flip (psx-tomba issue 0038).

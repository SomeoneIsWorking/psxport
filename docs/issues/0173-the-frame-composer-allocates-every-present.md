# 0173: the frame composer allocates every present

Status: fixed (pass 6). Owner: `runtime/psx/present/frame_composer.cpp` `FrameComposer`, `runtime/psx/frame/frame_presenter.cpp`.

## Observed
Review of psx-spyro issue 0196 pass 3: `composeFrame` builds its working containers afresh on every present, real
and in-between. Review of pass 4: every producer save and render allocates too. Spyro's twelve producer saves (Spyro 1's flame
`drawFlame` among them) build a `LevelReads` (an `unordered_map`) and its `words()` vector on each save, and each
render builds a `HostMemory` (a `std::map` of byte vectors) inside `actors::InBetween`.

## Cause
Per call it constructs `covered` (each object with an entry vector and an `unordered_map` of slot vectors),
`coveredIndex`, `clutOffsets`, `baked`, `SlotPlacement`'s three maps, `environmentsAt`, `removed`, `inserted`,
`emittedInSlot`, `placed` and `composed`, and copies `shown` into a new `FrameRecord`, which
`FramePresenter::composedStore_` frees after each build. The render list itself is reused since issue 0169's review
(`FramePresenter::renderScratch_`).

## Proper fix
A `FrameComposer` owned by `FramePresenter` that keeps every container as a member, cleared per compose, replaces
the node-allocating maps with sorted vectors, and composes into a kept `FrameRecord` whose vectors keep their
capacity (`replaceEntries` taking a span). Record sealing and `FrameState` also allocate per frame; the same
treatment applies there. Producers get the same: one `LevelReads` and one `HostMemory` per producer, kept by the
title's session and cleared per use, `LevelReads` noting into a vector sorted and deduplicated in place (its
`words()` a span over it) and `HostMemory` keeping its ranges in a sorted vector whose byte buffers are reused.

## Fix (psx-spyro 0196 pass 6)
- `present::FrameComposer`, owned by `FramePresenter`, keeps every working container as a sorted-vector member
  cleared per compose and composes into a kept `FrameRecord` that shares `shown`'s pools (`assignPools`).
- `FramePresenter` seals into a fixed pool of record slots (`slots_`, history plus two) instead of a new record per
  commit; `Gp0RecordTap::seal` swaps into the slot and `FrameRecord::reset` clears the tap's record, keeping capacity.
- `FrameState` and `FrameStates` keep sorted vectors and a double-buffered byte arena compacted at `endFrame`;
  `FrameStates::save` takes the parts of a state, so a producer saves a header and its reads without joining them.
- `LevelReads` is an open-addressed table over a first-read-order vector, `words()` a span; `HostMemory` keeps a
  sorted vector of ranges whose byte buffers return to a spare pool on `clear()`; `overlay` takes packed words.
- `present::Kept<T>` is scratch a title keeps for its producers; a `Kept<T>::Use` clears it and aborts on a nested
  use. Spyro's session keeps one `LevelReads` for saves and one `HostMemory` for in-between runs.
- `OtTables` keeps a packet's assigned bucket in a vector sorted in place when a walk begins (`beginWalk`) instead of
  an `unordered_map`, which allocated a node per packet the depth bins assign.
- `FrameStates::save`'s single-value overload is constrained to non-ranges: an array of parts once matched it and saved
  the spans themselves (48 bytes), which Spyro's terrain refused (`a saved terrain field of 48 bytes is not one`).

## Proof
gdb counting every `malloc` for 10 s of gameplay, grouped by the first project frames (`scratch/judder-pass6/tools`,
not committed). Spyro 1 `warp 10` on the final build (`alloc-s1c`, load 1.84-3.17): no allocation from `FrameComposer`, `FramePresenter`, `FrameStates`,
`HostMemory`, `LevelReads`, `OtTables`, any producer or the terrain, where the first run (`alloc-s1`, before the OT and terrain
changes) had 26120 from `OtTables::assign` and 78 from the terrain in-between's bin list. Spyro 2 `warp 21` (`alloc-s2`):
8 from `FrameComposer::compose` and `FrameRecord::replaceEntries`, capacity growth to a larger frame, none from the
producers. What still allocates is guest execution, issue 0175.

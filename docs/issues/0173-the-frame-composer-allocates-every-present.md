# 0173: the frame composer allocates every present

Status: open. Owner: `runtime/psx/present/frame_composer.cpp` `composeFrame`, `runtime/psx/frame/frame_presenter.cpp`.

## Observed
Review of psx-spyro issue 0196 pass 3: `composeFrame` builds its working containers afresh on every present, real
and in-between.

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
treatment applies there.

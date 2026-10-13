# 0152: the in-between splice drops a producer's extra primitives

Status: fixed

## Observed
Spyro 1 (`SCUS_942.28`) through the player build and environment (`build/player/bin/spyro_port`, no
arguments, `tools/shipping_settings.ini`: aspect=1, fps60=1), picker, `pick spyro1`, the user's own pad
recording replayed. `preseq 200` dumps 42 s after the pick, in Artisans: 27 of 100 in-between presents had
more than 1% pure-black pixels inside the picture (worst 40%), every real present under 0.11%. The holes are
the nearest ground faces, with a polygon-edged top, so the ground flashes black at 30 Hz.
`PSXPORT_FPS60_TFORCE=1` (in-between at t=1) removes them; `PSXPORT_FPS60_TFORCE=0` (t=0) keeps them.

## Cause
`runtime/psx/present/in_between_splice.cpp:spliceInBetween` gave each reconstructed primitive the slot of
the n-th captured primitive of its producer and dropped every primitive past that producer's captured count.
A sample culls on its own projection, so Spyro 1's world (`0x800258F0`) emitted e.g. 681 faces at t=0.5
against 672 captured, and the faces it emits last are its near-refined ones. Those were the ones dropped.

## Resolution
A producer's extra primitives follow its last captured slot and adopt that slot's `layer`, `draw_seq` and
`flush_ordinal`, so the planner's order and epoch checks still hold. Only a producer with no captured slot
at all is dropped (`surplus`). The census reports `appended` separately. Test:
`test_in_between_splice` `a_producers_extra_primitives_follow_its_last_slot_and_a_shortfall_is_declined`
(failed before: `census.appended` 0, want 2).

Same measurement after: 0 of 100 in-between presents over 1% black, worst 0.08%; three-present A-B-A
pixel flicker mean 1.94% -> 0.35%, worst 35.5% -> 3.0% (the remainder is fast motion). Picker preview
census: 3513 dropped surplus primitives -> 3440 appended, 0 surplus.

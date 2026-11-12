# 0201: a replaced VSync wait serviced the CD drive once per field

Status: fixed

## Observed
Mega Man X4 (`SLUS_005.61`), the user's pad recording of the opening movie (bug report
`20261112-153842-fmv-stutter`), `PSXPORT_DEBUG=cdcpace`: the movie picture changed every 8-9 pad frames (about
7 fps against the stream's 15), and sector events were serviced about 566,000 ticks apart, one per display field,
although each was armed 225,792 ticks (one double-speed sector period) after the last.

## Cause
`runtime/psx/frame/timing.cpp:Timing::advanceDisplayFields` moved the clock straight to the field boundary and
serviced the CD controller once, so every sector due inside a replaced VSync wait collapsed into that one
service and the guest's INT1 handler saw one sector per field.

## Resolution
`EmulatedTime::displayWaitEndTicks` plans the wait end with the same planner `advanceDisplayFields` uses, and
`Timing::serviceDeviceEventsBefore` walks every CD deadline before it: the clock advances to each, the controller
is serviced and a pending enabled interrupt is delivered before the clock moves on. After: exactly 4 fields per
movie frame and 150.00 sectors/s on the same recording.

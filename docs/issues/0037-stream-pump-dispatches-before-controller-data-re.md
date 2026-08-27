---
id: 37
title: Stream pump dispatches before controller data-ready
status: resolved
symptom: Continuous libcd stream callback runs once with no current controller response, then the genuine INT1 arrives empty and the guest stops yielding
tags: cdc,cdrom,streaming,callback,tomba1
created: 2026-08-27
updated: 2026-08-27
---

## Root cause

`Cd::pumpStream` treated its wall-clock sector budget as permission to dispatch the guest's ready
callback. It forced the first callback at elapsed time zero and incremented `stream_delivered` even
when the native controller had no current response. Tomba 1 then entered `StCdInterrupt` with
CdReady reason 0 and no data, before the controller's first scheduled sector became current. The
next callback observed the real INT1 but an empty data transition and stopped yielding after field
540.

## What was tried / dead ends

Returning the title's early-exit reason 5 for the empty callback would fabricate guest policy.
Decrementing `stream_delivered` after the callback would repair only the host counter, not the
controller/ring ordering already consumed by `StCdInterrupt`. Both are title-specific symptom
patches. The controller response, not a pacing estimate, must authorize callback delivery.

## Resolution

The shared pump now services the controller's deterministic drive clock and dispatches/counts a
stream callback only while `cdc_current_irq_type` reports current INT1 data-ready. A shipping-path
test proves all three answers: an empty queue does not invent the first callback, a non-data response
is not misclassified, and a current INT1 dispatches exactly once while preserving interrupted guest
registers. Tomba 1 real-disc re-verification is still required before this issue is resolved.

### Resolution (2026-08-27)
Focused shipping tests pass all three controller-response answers; isolated Tomba1 PID 3146271 has no forced reason0 callback, begins with genuine INT1 data=0/2048 at LBA57830, and advances through nine INT1 callbacks to LBA57838. The later movie busy-poll is a separate title frontier.

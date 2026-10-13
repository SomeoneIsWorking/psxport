# 0155: push-mode XA played three percent flat at every stream start

Status: fixed

## Observed
Spyro 1, after 0153. Each XA stream started 2.6% flat (about 45 cents) and drifted toward pitch over tens of
seconds: per-second consumption 36,846 -> 37,131 source frames/s against 37,800, ring backlog climbing
3,300 -> 12,300 frames.

## Cause
`runtime/psx/audio/xa_stream.cpp:CDC_GetCDAudioSample` trimmed the push-mode rate toward a half-ring backlog
(32,768 frames). A stream starts with an empty ring, so the error began at -0.5 and slowed playback by 3%
until the backlog was built from that slowdown alone, about 30 s.

## Resolution
`runtime/psx/audio/xa_push_pacing.h` owns the policy: playback holds until two stereo sectors (4,096
frames) are buffered, or for as long as that backlog would take, then the rate is trimmed toward that
backlog with the same gain. The ring re-primes after it starves. Test: `test_xa_push_pacing`
`a_stream_fed_at_the_disc_rate_plays_at_its_true_pitch` (failed before) and
`a_clip_shorter_than_the_backlog_still_plays`.

After: title stream -0.01%, level stream +0.03 to +0.07%, backlog 2,900-5,100 frames.

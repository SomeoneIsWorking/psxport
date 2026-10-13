# 0156: the audio queue never steered toward its cushion

Status: fixed

## Observed
Spyro 1 sound effects ~265-320 ms late for the whole session in the player build (mapped window,
`pick spyro1`, the user's pad). Stream queue (`SDL_GetAudioStreamQueued` after each put) in the same
route on the tree before this fix, 60-70 s each:

| title | load | steady queue mean (min-max) | drops |
|---|---|---|---|
| Spyro 1 | 3.2-6.9 | 123 ms (75-182), drifting 154 -> 104 | 0 |
| Spyro 2 | 1.8-3.5 | 207 ms (181-238) | 168 in the boot |
| Crash Bash | 0.8-1.6 | 157 ms (103-188) | 0 |

The device adds one 1024-frame period (23 ms at 44.1 kHz, `spu_audio` logs it at open) and PipeWire's
1024-frame 48 kHz quantum (21 ms). Before Spyro 1's post-pick burst was fixed (spyro 0183) the queue
sat at 235-255 ms after filling to the 360 ms limit, ~290 ms at the speaker.

## Cause
`runtime/psx/audio/audio_queue_policy.h:AudioQueuePolicy` had no restoring force: playback started at a
180 ms cushion, a burst raised the queue to the 360 ms drop limit, a hitch lowered it, and the level
then stayed wherever the last disturbance left it, since producer and device run at the same nominal
rate.

## Resolution
One target level, 90 ms: four NTSC fields of producer gap plus one device period. Playback primes and
re-primes to it; `AudioQueueSteer` smooths the post-put level over about a second and sets
`SDL_SetAudioStreamFrequencyRatio` proportionally, 1 at the target and at most 0.5% (8.6 cents) at an
empty queue or at the drop limit, now 180 ms. A queue at the limit converges in ~20 s. The WAV
capture is guest-time PCM and is unaffected. Reaches every title through `SpuAudio`.

Test: `tests/test_audio_queue_policy.cpp` `a_full_queue_converges_to_the_target`,
`a_drained_queue_refills_to_the_target` and `steady_state_holds_the_target_through_clock_drift` drive
the shipping steer against a period-pulling device model (all three failed with a unity ratio).

## Measured after
| title | load | steady queue mean (min-max) | drops | ratio range | queue at empty |
|---|---|---|---|---|---|
| Spyro 1 | 1.5-2.7 | 74 ms (17-115) | 0 | 0.9976-1.0000 | 2 puts, level-flyby load (99 ms gap) |
| Spyro 2 | 1.4-2.0 | 82 ms (21-120) | 0 | 0.9975-1.0003 | 0 |
| Crash Bash | 3.2-7.0 | 80 ms (35-118) | 0 | 0.9984-0.9999 | 1 at boot, 1 at a 114 ms gap |

## Open
- A producer gap above ~90 ms empties the queue (level loads above); the old 120-210 ms queue absorbed
  those. A stretch where the host runs under real time (Spyro 3 at load 4-6, 37,000-40,000 samples/s
  for 6 s, a ~700 ms deficit) empties either.
- `SpuAudio::mFieldReports` keeps every field's PCM for the whole session (~180 KB/s).

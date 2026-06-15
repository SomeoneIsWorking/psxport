# audio_differ — capture & compare the port's SPU audio

The SPU lift (`runtime/recomp/spu_beetle.c`) is Beetle/mednafen's own `spu.c`, so a faithful
run should mix the *same* PCM the oracle would — given the same register/DMA inputs and the
CD-DA source wired. This tool is the listening/measuring end.

## Capture our output
`PSXPORT_WAV=path` dumps the SPU's mixed 44100 Hz stereo int16 to a WAV. It is **independent of
SDL** — it works headless and under `PSXPORT_NOAUDIO` (the SPU is still advanced + drained, the
samples are just written to disk). Header sizes are patched at exit; capped at ~10 min.

```
PSXPORT_NOWINDOW=1 PSXPORT_NOAUDIO=1 PSXPORT_NO_FMV=1 PSXPORT_FORCE_BUTTONS=FFF7 \
PSXPORT_NATIVE_FRAMES=3300 PSXPORT_WAV=scratch/wav/ours.wav ./run.sh
```

## Inspect / compare
```
tools/audio_differ/compare.py stats scratch/wav/ours.wav          # duration, RMS/peak, silence %
tools/audio_differ/compare.py diff  scratch/wav/ours.wav oracle.wav   # xcorr-align + similarity
```
`stats` flags an all-zeros (silent) capture outright. `diff` cross-correlation-aligns the two
(our HLE port and the oracle run at different timings, so they are NOT sample-aligned — same trap
the GPU differ's input-replay sidesteps) then reports loudness/peak deltas at the best lag.

## SPU triage (`PSXPORT_SPU_DBG=1`, spu_beetle.c)
Logs whether the game actually drives the SPU: total register writes, SPUCNT (enable + transfer
mode), KON key-on masks, SPU-RAM DMA/data-port transfers, and per-frame `spu_render` peak. Use it
to tell "SPU mixed silence" (a mixing bug) from "the game never keyed a voice / never uploaded
samples" (a higher-level driver/IRQ problem).

## Oracle capture (the ground-truth reference)
`wide60rt` (full Beetle) also honors `PSXPORT_WAV` (added in `runtime/main.cpp`), headless:
```
PSXPORT_WAV=scratch/wav/oracle.wav runtime/wide60rt <disc.chd> -bios scratch/bios \
  -frames 9000 -inputscript scratch/inputs-skipintro-long.txt
```
`PSXPORT_SPU_DBG=1` on `wide60rt` (instrumented `mednafen/psx/spu.c`) logs oracle KON (with the
writer's CPU PC), SPUCNT, and a per-10s `CDmix` counter (CD-audio-enabled / nonzero samples).

## Root cause (journal later-53 — corrects later-52)
The port's silence is **candidate #3, not #1**: Tomba2's in-game/menu music is **SPU-voice
(KON-driven) and runs inside a Timer/SPU-IRQ handler** that fires during the per-frame pace-dwell
(oracle KON writer PC = the dwell loop `0x80050CE4`). The port models only VBLANK and delivers no
Timer/SPU IRQ (and collapses the dwell), so the sequencer ISR never runs → zero per-note KON →
silent SPU. FMV/XA audio is separate (`native_fmv.c`, own SDL device) and already works; the
stubbed `CDC_GetCDAudioSample` is NOT the menu/gameplay-music gap. Fix = run the sequencer tick
per frame at the timer rate (see later-53). Not yet implemented.
```

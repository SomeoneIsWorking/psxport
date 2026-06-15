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

## Known gap (journal later-52)
The port is currently **silent** in headless gameplay. `PSXPORT_SPU_DBG` shows the SPU is enabled,
master volume set, and SPU RAM uploaded — but only the all-voices init KON fires (5 in 2500 frames);
**no per-note key-ons**, i.e. the game's sound driver isn't sequencing. Plus CD-DA (streamed music)
is stubbed to silence (`CDC_GetCDAudioSample`). Fixing audio = drive the sound-engine update / wire
its IRQ + connect a CD-DA source — a subsystem task, not yet done.
```

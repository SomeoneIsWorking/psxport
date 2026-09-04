---
id: C033
kind: claim
status: holds
created: 2026-08-24
tags: audio,timing,cadence
depends: runtime/psx/field_rate.h#DISPLAY_FIELD_RATE_NTSC, runtime/psx/spu_field_cadence.h#SpuFieldCadence, runtime/psx/spu_audio.cpp#SpuAudio::frameEx, tests/test_spu_field_cadence.cpp
---

## Claim

The SPU audio sink converts the exact display-field rational to SPU clocks and 44.1 kHz sample frames with carried integer remainders, so 60,000 NTSC fields produce exactly 44,144,100 frames over 1,001 seconds.

## Evidence

test_spu_field_cadence passed 17 checks through SpuFieldCadence: exact 60 Hz preserved 564,480 clocks/735 frames per field; 60,000 fields at 60,000/1,001 Hz totaled 33,902,668,800 clocks and 44,144,100 frames with bounded 565,044/565,045-clock and 735/736-frame steps; NTSC-to-PAL reset produced exact 677,376 clocks and 882 frames. Clang 22.1.8 built all targets and full CTest passed 96/96 including cpp_style.

## What would falsify it

If SpuAudio::frameEx stops taking its clocks and render bound from SpuFieldCadence, if the display field-rate authority changes without the cadence following it, or if the deterministic long-run totals test fails.

---
id: 45
title: Texture modulation rounds instead of truncating
status: resolved
symptom: Dark textured faces contribute one extra 5-bit step per channel in opaque and semitransparent Vulkan paths
tags: gpu,renderer,texture,modulation,quantization,crashbash
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

Both textured Vulkan fragment paths rounded the modulated 5-bit texel back to 5-bit. The PSX
multiplies each texel channel by the interpolated 8-bit color and truncates the integer division by
128. For texel `(1,1,1)` and color `(74,74,74)`, retail therefore contributes `(0,0,0)` while the
shader contributed `(1,1,1)`. Additive semitransparency made the error one 5-bit step brighter in
every channel.

Crash Bash source pixel `(238,61)` supplies the exact witness. The underlying G3 face and the
semitransparent textured face both have exact native/packet geometry and colors. Retail packet
`0x800C7454` samples texel `0x8421`, truncates its modulation to zero, and leaves background `0x0C01`
unchanged; the native result was `0x1022`.

## Resolution

`tritex.frag` and `trisemi_hw.frag` now reconstruct the interpolated color as 8-bit and perform the
modulation with unsigned integer multiply/divide before saturation, matching the software rasterizer's
`(texel8 * color8) / 128` truncation in `gpu_native.cpp`.

`gpu_vk_modulation_selftest.cpp` owns the discriminator separately from the blend-equation matrix, which
keeps `gpu_vk_semi_selftest.cpp` at its 16 raw-texel cases. It stages the same dark texel three ways and
seeds every destination so a case that never rasterized cannot read as a pass: ABR1 additive (the retail
witness — the background must survive), ABR0 average (a zero contribution still halves the destination,
which proves the draw fired), and opaque (the modulated texel is written verbatim, so a seeded background
must go to black). It passes 3/3 through the shipping path, and reverting only the two shaders makes it
report `1022 / 0801 / 0421` against expected `0C01 / 0400 / 0000` — the additive cell reproduces the exact
Crash Bash witness pair.

Measured on the consuming port: Crash Bash's exact frame-300 diff>8 against its retained PSX reference
falls from 5,546 to 45 of 691,200 pixels (upper 39, subtitle 0, lower 6), on a 301-frame run that
reconciles every frame with no executor fault, fatal trap, or guest-VSync violation.

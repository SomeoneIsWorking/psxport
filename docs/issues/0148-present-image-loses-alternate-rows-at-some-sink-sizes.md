# The presented image loses alternate rows at some sink sizes (unfixed, driver-sensitive)

Status: **open, not being fixed now.** Measured 2026-10-02 on C-12. A real defect in what a capture
reports, bounded to a size/driver combination, with every input to the present pass measured
correct. Recorded so it is not re-derived; no fix is claimed and no mechanism is claimed.

## What is seen

`present_shot()` returns an image whose alternate rows are missing or dimmed, for some
`PSXPORT_PRESENT_SINK` sizes, on this host. RGB-only row means (alpha excluded — the alpha channel
is 255 on the missing rows, which is the present pass's clear colour and is what made this look
like a "lost raster line" for a long time):

| sink | RGB even | RGB odd | odd/even | verdict |
|---|---|---|---|---|
| 512x240 | 0.0990 | 0.0000 | 0.000 | every odd row missing |
| 1280x720 | 0.0793 | 0.0397 | 0.500 | odd rows at half brightness |
| 512x480 | 0.0631 | 0.0633 | 1.004 | balanced |
| 512x240 on llvmpipe | 0.0139 | 0.0146 | 1.047 | balanced |

So it is size-dependent, and on this host it does not reproduce on llvmpipe. That is a measurement,
not a driver diagnosis: no mechanism is identified, and "radv does X" is not a claim being made.

## What is NOT the cause (all measured)

- **The composite texture is whole.** Downloaded in full (1024x512) with both interlaced field
  windows — the guest presents `disp.y` alternating 0 and 256 — every one of the 240 rows in each
  window has content: even field 4740/3899, odd field 1379/486.
- **The present pass geometry is correct.** Target created 512x240 `R8G8B8A8_UNORM`; viewport
  (96,0,320,240); scissor (0,0,512,240); source rect `disp=(0,0,320,240)`. 1:1, no height mismatch,
  no sink-size scale, no interlace term in either shader.
- **The fragment shader samples correctly.** Presented row N matches composite row N (mean |Δ|
  0.0-2.5 on frames where the scene had not moved), not row 2N. It is not a stride-2 fetch.
- **It is not the capture's synchronisation.** Recording the present image inside the frame's OWN
  command buffer immediately after `build_present_image()`, and comparing it against what the
  capture reads after the frame's fence, gives byte-identical results. Waiting on an empty
  submit-and-wait, or on the fence of the present's own submit, changes nothing.
- **It is not the copy parameters or the destination buffer.** Four shapes of the same download
  (varying `pixels_per_row`, `rows_per_layer`, and transfer buffer) and two consecutive downloads
  each with their own submit-and-wait all return the identical image.
- **It is not the sink blit.** `plan_present()` sets `to_swapchain = !headless`, so
  `show_present_image()` never runs in the headless leg this was measured in.
- **It is not frames in flight.** Paced and unpaced runs are byte-identical.

## What this invalidated

A previous change made `upload_vram()` copy the whole canvas for titles whose guest VRAM is the
whole picture, on the claim that a short-extent `SDL_UploadToGPUTexture` leaves alternate rows
unwritten. That claim was measured entirely through `present_shot`, i.e. through this defect. With
that change reverted, the per-region copy and the whole-canvas copy produce a **byte-identical**
`s_present_img` (same SHA-256), so the copy extent makes no difference to the picture. The change is
reverted; this issue is what remains.

## Acceptance criteria for a fix

1. A capture at 512x240 is parity-balanced by the RGB-only row metric above, on the host that shows
   the defect, **and** the composite texture is still whole.
2. The fix does not depend on `PSXPORT_PRESENT_SINK` — 512x240, 512x480 and 1280x720 all balanced.
3. It does not regress any title whose picture is currently correct; re-measure C-12 at all three
   sizes plus one Gte title.

## How to reproduce

    PSXPORT_VK_HEADLESS=1 PSXPORT_NOAUDIO=1 PSXPORT_NOPACE=1 \
    PSXPORT_NATIVE_FRAMES=1402 PSXPORT_PRESENT_SINK=512x240 PSXPORT_PRESENT_SHOT_AT=1400 \
    ./build/maintainer/c12_port <disc>

Then compare even/odd row means over RGB only. Measuring over RGBA hides this, because the
missing rows carry alpha 255.
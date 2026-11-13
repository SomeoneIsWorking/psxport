# 0205 — Hybrid crop + extend widescreen

User, 2026-11-13: "The crop idea won't work well in wider resolutions so maybe we can add hybrids
(combining crop and extending)".

## Shape

For sinks wider than 16:9, crop the picture to an intermediate aspect and extend the sides for the rest,
so neither the crop nor the margins grow extreme. Builds on plan 0203's per-layer mappings (world band,
UI box) and the per-record view/drawn windows (`runtime/psx/present/horizontal_margins.h`). Offered only to
titles that support both crop (`CropUiLayers`) and extend.

## Progress

Not started.

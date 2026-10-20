# 0163: the selector magnified each panel's picture instead of showing it at the resolution selected

Status: fixed

## Observed
Spyro's title selector was visibly low-resolution. A controlled A/B at the same demo moment, same
binary, changing only the RmlUi menu's Internal Resolution between Vanilla (1x) and X4, showed
essentially no difference in the presented picture.

At a 3840x2160 sink with three panels the measurements were:

- pane image 1920x2160, guest picture viewport 1920x1077 inside it, panel cover 2112x2160
- `src_ires=4` on every pane present, so the internal resolution setting WAS reaching the raster

So the knob was not the defect. The compositor was: `PickerComposite::build` cover-cropped the
picture to the panel's aspect and drew that crop into the panel's cover. A 16:9 picture cropped to a
0.73 panel keeps 54.9% of its width, so a 1054x1077 crop was stretched to 2112x2160 — a 2.0x
magnification of an already-1x render, which reads as blur at any internal resolution.

A second, smaller stretch was upstream of it: the pane image was sized to the panel's *tiling share*
(1920) while the panel's *cover* is 2112 wide, because a cover overlaps its neighbour by up to one
slant. The picture was magnified 1.1x before the crop was even applied.

## Cause
Two owners answered "how much of the picture does this panel show" and disagreed with each other and
with the rasterizer:

- `PickerComposite::setSurface` sized every session's present image to `PickerLayout::maxPanelWidth`,
  the panel's share of the window, not the size the panel is drawn at.
- `PanelLayout::sourceCrop` measured the crop against the panel's average width while the pane is
  drawn into its wider cover rectangle, so the kept slice was stretched by cover / average width.

## Resolution
- The selected panel takes 60% of the window's width and the others split the rest evenly
  (`picker_selected_share` in the settings file; `PickerLayout::retarget`). The shares were two
  ratios normalised by their total, so 0.6:0.2 gave two titles 75/25; there is now one rule, and the
  hidden-window selector logs `panel widths 2304 1536` at 2 titles and `2304 768 768` at 3 (3840 wide).
- The selected panel's present image is the window's size (`PickerComposite::paneImage`), so its
  picture is rendered at the display's own resolution and the panel shows its cover's share of it one
  pixel for one. The other panels are paused thumbnails and present at a third of their own cover.
- A pane's present is filled rather than letterboxed (`PresentInputs::fill`): the picture is
  centre-cropped to the image's shape at its displayed 4:3 aspect, at the rendered resolution.
- `PanelLayout::sourceCrop` crops against the cover rectangle the pane is drawn into, so a picture
  keeps its aspect whatever size it was built at, including a paused panel's last picture.
- Auto could not reach a 4K display: `videoStatus`'s budget was 128 MiB, which caps `ires` at 4,
  while a 2160-line window asks for 9. The budget is 640 MiB (admits 9; `kIresCeiling` caps at 8),
  and a pane measures its own present image rather than the window.
- `PickerLayout::maxPanelWidth` had no remaining caller and was removed.

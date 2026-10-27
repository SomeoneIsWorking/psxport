# 0179: journal rows lack routes and overlay images in most titles

Status: open. Owner: each title's `TitleDebugMenu` (`screen`, `decompImage`).

## Observed
An owner journal row is a self-contained task only when its title names the screen's route and the decomp image of
the code that drew it. On 2026-10-27 only Spyro (title, level, Spyro 1 pause), Tomba 2 (field areas, pre-GAME stages)
and the test program route their screens; every other title's rows carry `screen=''`. The default `decompImage`
covers the resident executable only; Tomba 2 and Toy Story 2 map their overlays to manifest entries. Rows drawn from
an overlay in C-12 (`c12_gt`), Crash Bash (`crashbash_boot`, `_menu`, `_dat28136`), CTR (`ctr_bf0226`, `_bf0231`)
and Vagrant Story (`vagrant_battle`, `_title`, `_initbtl`) get no decomp command.

## Next
Per title: a screen key from the guest's own mode/stage words and a route through its existing warp; a
`decompImage` override naming the manifest entry and provisioned file for each catalog overlay name.

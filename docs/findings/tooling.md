# Findings — tooling / debug server / harness

## A debug-server client disconnect kills the whole game (SIGPIPE)
- **symptom:** the live port dies ("connection refused" on the next dbgclient call) when a debug client is killed / times out mid-reply — looked like a crash on entering a scene, but no abort/diagnostic is printed.
- **status:** fixed 10a07e0
- **cause:** the debug server's socket write() raised SIGPIPE with the DEFAULT disposition (terminate the process); a dropped/timed-out dbgclient connection therefore killed the game.
- **fix:** dbg_server.cpp dbg_server_start: signal(SIGPIPE, SIG_IGN). A dropped client now just makes write()<0 and drops that one connection.
- **refs:** journal later-252; dbg_server.cpp dbg_server_start

## Reaching the New-Game cutscene live: mash START (it confirms the menu AND skips FMV)
- **symptom:** automated drive (debug-server taps) loops in the attract demo / never reaches the field cutscene (stage 0x8010637C); the title menu's confirm wasn't registering.
- **status:** known-good (how-to)
- **cause:** the real-time menu→cutscene cadence; the attract demo replays OP.STR if no valid selection lands.
- **fix:** drive: tap START at the title demo (→ menu, scene-active=2), then mash START — it both confirms New Game and skips the FMV — and poll `stage` (debug server) for 0x8010637C. The debug server now survives client drops (SIGPIPE fix above) so a `timeout`-killed dbgclient won't kill the game. Headless can't reproduce the menu cadence — needs the windowed game.
- **refs:** tools/dbgclient.py; docs/driving-the-game.md

## SBS both-mode: panes black + silent on macOS/MoltenVK (fine on linux/AMD)
- **symptom:** `PSXPORT_SBS_MODE=both ./run.sh` reaches free-roam but BOTH panes are black and there's no audio, on Apple M2 / MoltenVK; only the RmlUi overlay shows. Plain `./run.sh` (single core) is visible AND audible on the same Mac.
- **status:** known-issue (MoltenVK-specific; needs Mac-side verification to fix)
- **cause:** localized, not root-caused. On linux/AMD (RADV) the SBS renders fine — both panes have content (verified: shot/shotb mean ~107, ~74k/76800 px nonzero, title/menu visible). So the SBS RENDER + targets work; the failure is the macOS path. The SBS uses a SEPARATE two-pane composite (gpu_vk.cpp present_sbs: per-pane staging buffers s_stage_ptr[0/1] via sbs_stage_b_ensure, per-panel descriptor sets s_panels[p].dset_present, panel_upload+panel_render per pane) — distinct from single-mode present() which works on the Mac. Suspect a MoltenVK incompat in that two-panel setup (2nd staging buffer / panel-B descriptor / sampler binding). Audio: SBS pumps via present_sbs, not gpu_present — likely not wired to feed the device (separate from the render bug; secondary).
- **fix:** NOT fixed. Needs a Mac to verify. Next: diff present_sbs vs present() for the MoltenVK divergence (descriptor/staging/sampler), or get Mac-side validation-layer output. Do NOT blind-fix from linux (can't verify). Exposed by later-253 (the freeze fix) — before that SBS never reached free-roam on Mac.
- **refs:** gpu_vk.cpp present_sbs / sbs_stage_b_ensure / panel_render; journal later-249 (other macOS/MoltenVK render quirk); later-253

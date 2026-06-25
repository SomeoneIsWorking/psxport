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
- **symptom:** `PSXPORT_SBS_MODE=both ./run.sh` reaches free-roam but BOTH panes are black at EVERY stage (title/demo AND field — confirmed by the user it's total black) and there's no audio, on Apple M2 / MoltenVK; only the RmlUi overlay shows. Plain `./run.sh` (single core) is visible AND audible on the same Mac.
- **status:** known-issue (MoltenVK-specific; needs Mac-side verification to fix)
- **cause:** localized, not root-caused. On linux/AMD (RADV) the SBS renders fine — both panes have content (verified: shot/shotb mean ~107, ~74k/76800 px nonzero, 2D title visible). So the SBS RENDER + targets work; the failure is the macOS composite path. PRIME SUSPECT: `panel_render` uploads each pane's VRAM into a SINGLE SHARED image `s_vram_tex` and samples it, and `present_sbs` records BOTH panes into ONE command buffer (pane 0 uploads A→s_vram_tex + samples; pane 1 then overwrites s_vram_tex with B + samples). RADV tolerates this serialized write-then-sample-then-overwrite of a shared image across two render-pass instances; MoltenVK/Metal's hazard tracking across encoders is far stricter and is the likely reason BOTH panes go black. (Single-mode present() touches s_vram_tex only once per frame, so it's fine on the Mac.) Other less-likely: per-panel dset_present / 2nd staging buffer. Audio: SBS pumps via present_sbs not gpu_present — likely just not wired to the device in SBS (secondary, separate from render).
- **fix:** NOT fixed; do NOT blind-fix from linux (can't verify a MoltenVK render). DIAGNOSTIC FIRST: run on the Mac with `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation PSXPORT_SBS_MODE=both ./run.sh` and capture the validation/sync errors — they name the exact hazard. CANDIDATE FIX (verify on Mac): give each pane its OWN vram-tex (s_vram_tex[2]) so the two panes don't share/overwrite one image in a single command buffer. Exposed by later-253 (the freeze fix) — before that SBS never reached free-roam on Mac.
- **refs:** gpu_vk.cpp present_sbs / sbs_stage_b_ensure / panel_render; journal later-249 (other macOS/MoltenVK render quirk); later-253

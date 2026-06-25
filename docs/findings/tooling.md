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
- **status:** ROOT-CAUSED + fixed (barrier READ access) — awaiting Mac confirm
- **cause:** ROOT CAUSE (found via sync validation, NOT a guess): `SYNC-HAZARD-READ-AFTER-WRITE` in `panel_render` (gpu_gpu.cpp). The panel render pass uses `loadOp = VK_ATTACHMENT_LOAD_OP_LOAD` (it preserves the just-uploaded VRAM backdrop, then draws geometry on top), so beginning it READS the color attachment. But the barrier transitioning that image TRANSFER_DST→COLOR_ATTACHMENT_OPTIMAL granted only `VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT`, not `..._READ_BIT` — so the loadOp-read was unsynchronized against the prior transfer-write. RADV ignores the missing READ (renders fine — which is why linux/AMD SBS worked + masked it); MoltenVK/Metal ENFORCES it, so the load read nothing → both panes black at every stage. THE PER-PANE s_vram_tex THEORY WAS WRONG (reverted): sync validation flagged NO hazard on s_vram_tex — that sharing was correctly barriered. (The other validation noise, VUID-vkCmdPushConstants-offset-01796 ×20 from overlapping FRAGMENT[0,64)/VERTEX[16,48) push ranges, is spec-pedantry MoltenVK's per-stage push model handles; NOT the black cause — left as-is.)
- **fix:** gpu_gpu.cpp panel_render: the TRANSFER_DST→COLOR_ATTACHMENT_OPTIMAL barrier's dstAccessMask is now `COLOR_ATTACHMENT_WRITE | COLOR_ATTACHMENT_READ`. Verified on linux: the SYNC-HAZARD validation error went from many→0 (`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1 PSXPORT_SBS_MODE=both ./run.sh`). DIAGNOSTIC LESSON: the validation layer IS installed on linux/AMD — run sync validation THERE to catch MoltenVK-enforced hazards that RADV silently tolerates, instead of guessing.
- **refs:** gpu_gpu.cpp panel_render barrier; journal later-253; the per-pane mis-fix was reverted (commit f395e1d → revert)

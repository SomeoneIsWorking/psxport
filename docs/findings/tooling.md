# Findings — tooling / debug server / harness

## recdep / any rec_dispatch hook is BLIND to resident→resident (intra-MAIN) calls
- **symptom:** `recdep` and ad-hoc `camtrace`-style hooks in `rec_dispatch` show ZERO calls to a resident MAIN function (e.g. the camera 0x8006E3B0/0x8006D02C) even while it demonstrably runs every frame — leading to a WRONG "that code never runs here" conclusion (cost later-290..292 chasing a phantom free-roam camera).
- **status:** known-issue / measurement caveat (2026-07-01, later-293)
- **cause:** the recompiler emits **intra-module (MAIN→MAIN) calls as DIRECT C calls** `func_XXXX(c)` (see generated/shard_*.c, shard_disp.c `func_XXXX`), NOT via `rec_dispatch`. `rec_dispatch` only fires for CROSS-module / overlay / unknown-indirect targets. So any hook in `rec_dispatch` (recdep histogram, camtrace) sees ONLY cross-module and indirect calls — it is structurally blind to the (large) set of resident→resident calls. Absence from recdep does NOT mean "not executed".
- **fix / how to actually tell if a resident fn runs:** don't infer from recdep. Use a GUEST-STACK backtrace at a known side effect (`PSXPORT_WWATCH=<addr>` on a memory location the fn writes + `guest_backtrace_to`, i.e. PSXPORT_WWATCH_BT), or instrument the recompiled `func_XXXX` / `gen_func_XXXX` directly, or grep the shards for `func_XXXX(c)` call sites. recdep remains valid for its stated purpose (ranking substrate/overlay dependency to minimize cross-module dispatch) — just never read "0 in recdep" as "dead".
- **refs:** runtime/recomp/overlay_router.cpp rec_dispatch (only cross-module); generated/shard_3.c:16799 func_8006E3B0(c) direct; docs/findings/camera.md (the false trail this caused).

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

## codemap.py: an orphan doc-comment block hides the addresses of the def below it
- **symptom:** after owning a function natively (correct code, builds, A/B 0-diff), `tools/codemap.py --addr 0x80025588` still prints "NO native owner found", and docs/code-map.md maps the new native symbol to a WRONG/neighboring guest address (with a mismatched description).
- **status:** worked-around (blank-line separation); underlying heuristic gap noted
- **cause:** codemap keys a native's primary address off the FIRST line (up to the first em-dash/colon) of the CONTIGUOUS `//` block immediately above its `void ov_xxx(Core*` def. A pre-existing *orphan* doc-comment block (documentation for a handler that has no function under it — e.g. the "GAME sm[0x4a]==1 handler 0x801088d8" block in engine_stage.cpp) that sits directly above your new function, with NO blank line, gets merged into your function's comment. The merged block's first line names the orphan's address, so THAT becomes the primary and your function's real addresses (which live on later comment lines, past the em-dash) are never indexed.
- **fix:** put a blank line between an orphan/standalone doc-comment and the next function's own comment, so codemap collects only the function's own block. Verify with `tools/codemap.py --addr <fn-addr>`. Proper long-term fix (not yet done): codemap should stop attaching a comment block to a def when the block's first line addr ≠ any address referenced in the def body, or should scan the whole block (not just the header line) for the impl address.
- **refs:** tools/codemap.py parse_file (comment collection ~L78-116) + impl-address selection (~L100-116); engine_stage.cpp ov_scene_25588/ov_scene_4fe84; journal later-289

## recdep hot-leaf leaderboard: entries may ALREADY be native-ported (ORPHAN) — check codemap FIRST
- **symptom:** the handoff points at the top substrate-dispatch entries by calls/frame (e.g. 0x8007778C @115k, 0x80077ACC @48k, 0x8002B278 @38k, 0x8004D7EC @36k) as "port these next". Session-2026-07-02 disassembled 0x8007778C to reimplement — only to discover `tools/codemap.py --addr 0x8007778C` already shows it as `ov_cull_wrapper` in game/render/cull.cpp, ORPHAN. Same for the other three. Wasted time on a redundant RE pass before the check.
- **status:** workflow rule (2026-07-02)
- **cause:** the leaderboard measures rec_dispatch hits at CALL SITES — a hot substrate leaf can appear either because it has no native port OR because a written-but-orphan native port isn't wired into its native callers. The two states look identical at the recdep layer.
- **fix / rule:** for EVERY frontier target the handoff (or recdep) lists, run `tools/codemap.py --addr 0xADDR` FIRST before disassembling. If it's already ORPHAN, the work is WIRING (grep the callers, replace `rec_dispatch(c, 0xADDR)` with the native call), not porting. Only if codemap says NO native owner do you disassemble and reimplement.
- **refs:** game/render/cull.cpp (ov_cull_wrapper / ov_cull_wrap_77acc / ov_cone_cull_2b278 all ORPHAN); game/math/mathlib.cpp ov_bittest_4d7ec (was ORPHAN, wired e9f4b1e); scratch/logs/recdep_new.log

## ORPHAN native with a `_verify` wrapper is NOT proof it's byte-perfect
- **symptom:** trying to wire ov_orch597AC (FUN_800597AC) at engine_submit.cpp:1211 tripped a 9-byte main-RAM A/B mismatch @0x1FFCC9 at frame 500 even though the native has an `orchverify` gate. Wire had to be reverted.
- **status:** workflow caveat (2026-07-02)
- **cause:** a `*_verify` wrapper (record_gate / an inline snapshot+super_call harness) means someone WROTE the compare scaffolding, not that the compare has ever RUN clean. The gate is off by default (cfg_dbg("orchverify") is 0 unless set), so the default path is unverified native. Wire-time A/B may still fail if the native impl and recomp body actually differ.
- **fix / rule:** the wire-time A/B (main-RAM + scratchpad cmp = 0) is the ONLY proof. If a wire trips a diff, revert immediately — the "byte-perfect" native was never actually proven. Optionally rerun with the gate on (e.g. `PSXPORT_DEBUG=orchverify`) to localize the divergence, but do NOT ship the wire.
- **refs:** attempted wire reverted 2026-07-02; game/render/engine_submit.cpp ov_orch597AC (orchverify gate); the earlier successful wires in this session (ov_unpack_group, ov_obj_pos_compose, spawn_dispatch, ov_obj_record_init, the collision-grid trio) all had A/B = 0.

# Findings — FMV / CD streaming

## SBS both-mode freezes on the intro OP.STR movie
- **symptom:** `PSXPORT_SBS_MODE=both ./run.sh` freezes after `[fmv] MOVIE/OP.STR -> LBA 152238`, spamming the game's own `time out in strNext()` (seconds apart); window stops updating. PSXPORT_NO_FMV=1 does NOT help.
- **status:** fixed 5a61c3d
- **cause:** OP.STR is owned by the native FMV player (already g_sbs-skipped, native_fmv.cpp:677 `if (g_sbs) return 0` — so core A is fine). Core B (PSX) runs the guest demo machine; its STR streamer strNext (FUN_8010755c) waits for CD-streamed STR sectors NEVER fed on the interp path, busy-polling StGetNext (FUN_8008d030) ~2000x2000 = ~4M interpreted times per attract cycle — a non-yielding multi-second spin that stalls the SBS lockstep.
- **fix:** sbs.cpp step_core: while a core is in the DEMO stage (0x801fe00c==0x801062E4) and its demo SM[0x48]==1 (intro-FMV sub-state), set the game's OWN skip flag DAT_1f80019d — the demo prologue forces the teardown path, skipping the async STR streaming synchronously. Confined to DEMO stage so the GAME-stage cutscene is never skipped.
- **refs:** journal later-253; sbs.cpp step_core; DEMO.bin.asm FUN_8010755c/FUN_801075f8

## CdSync (FUN_8008a6ec) is already HLE'd to report completion
- **symptom:** worry that a `do { FUN_80089e1c(9,0,0)=CdControlB(CdlPause) } while(==0)` teardown loop (or any CdControlB-blocking) will spin forever under the no-IRQ runtime.
- **status:** known-good (do not re-investigate)
- **cause:** CdControlB blocks on CdSync (FUN_8008a6ec), which busy-loops on the CD-IRQ status (never set here).
- **fix:** Already handled — cd_override.cpp:397 platform_hle_register(0x8008A6EC, ov_cd_sync) sets v0=2 (CdlComplete). So CdControlB-blocking commands COMPLETE. The platform-HLE table is consulted on the interpreter path (interp.cpp coro_native_call) for every jal; runtime is interpreter-only (dispatch.cpp rec_func_index always -1). Faking StGetNext frames is a MINEFIELD (re-enters the decode-wait spin FUN_80106f80 case 6 `do{}while(_DAT_1f800034==0)` and corrupts SM[0x4a]); use the demo skip flag instead.
- **refs:** cd_override.cpp:397 ov_cd_sync; sync_overrides.cpp; interp.cpp coro_native_call

// Native CD overrides (S3): replace the game's libcd read path with direct native file
// I/O, completing synchronously. "No emulation" — we do NOT model the CD controller or
// deliver CD IRQs; instead the recomp-overrides layer swaps in native bodies for the two
// chokepoints, keeping the original recomp bodies alive (A/B + diffable) per the skill.
//
// Override points (entry addresses; decomp = scratch/decomp/ram_f1000_all.c):
//   * 0x8008B2D8  FUN_8008b2d8  low-level CdInit. The real body does the CD-controller
//     reset handshake then spins in CD_cw / CD timeout waiting on the IRQ-set status
//     DAT_800ac298, which nothing sets -> Init failed. Override: report drive ready
//     (v0=0). Its caller FUN_80089930 still runs FUN_8008b19c (harmless reg pokes) and
//     FUN_800898a0 still installs the libcd callbacks, so only the HW handshake is bypassed.
//   * 0x8008C1EC  FUN_8008c1ec(blocks, lba, buf) the single synchronous read primitive
//     (LBA->MSF, Setloc, ReadN, blocking wait via FUN_8008cafc). Override: read blocks*2048
//     bytes from the disc image at lba straight into buf, return 1 (its bool success value).
//     This bypasses the whole FUN_8008c960/c5d8/cafc/ac34 command+IRQ machinery for data.
#include "core.h"
#include "game.h"
#include "c_subsys.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>

enum { V0 = 2, A0 = 4, A1 = 5, A2 = 6 };


// Native in-game XA-ADPCM streaming (xa_stream.c). The CdControl wrapper below feeds it the
// streaming commands the game uses for cutscene BGM / voice (Setmode XA bit, Setloc, ReadS).

// 0x8008B2D8 FUN_8008b2d8: low-level CdInit -> success (drive ready), no HW handshake.
static void ov_cdinit(Core* c) { c->r[V0] = 0; }

// libcd command/sync primitives whose real bodies spin in a CD_cw / CD timeout loop on the
// IRQ-set status DAT_800ac298 (never set — no controller). Since every DATA read is served
// natively by ov_cd_read, the remaining CdControl/CdSync calls (Setmode, Pause, Nop, ...)
// only need to report success so their waits fall through. We replace, not super-call: the
// real bodies cannot return without the IRQ. Result bytes (drive status) the caller may copy
// are zeroed — callers on the boot path branch on the return value, not the status bytes.
static void zero_result(Core* c, uint32_t p) { if (p) for (int i = 0; i < 8; i++) c->mem_w8(p + i, 0); }

// 0x8008AC34 FUN_8008ac34(cmd, param, result, mode) CdCommand -> 0 (success).
static void ov_cd_command(Core* c) {
  if (cfg_dbg("cdcmd")) {
    uint32_t cmd = c->r[A0] & 0xFF, param = c->r[A1];
    uint8_t p[4] = {0,0,0,0};
    if (param) for (int i = 0; i < 4; i++) p[i] = (uint8_t)c->mem_r8(param + i);
    fprintf(stderr, "[cdcmd] cmd=0x%02X param=[%02X %02X %02X %02X] mode=%u ra=0x%08X\n",
            cmd, p[0], p[1], p[2], p[3], c->r[7], c->r[31]);
  }
  // In-game XA-ADPCM streaming: the game drives cutscene BGM / voice through these controller
  // commands. We don't model the controller (data is read natively elsewhere), so route the
  // streaming-relevant ones to the native XA engine; everything else still just ACKs.
  uint32_t cmd = c->r[A0] & 0xFF, param = c->r[A1];
  uint8_t p0 = param ? (uint8_t)c->mem_r8(param) : 0;
  switch (cmd) {
    case 0x0E: xa_stream_setmode(p0); break;                                  // Setmode
    case 0x0D: xa_stream_setfilter(p0, param ? (uint8_t)c->mem_r8(param + 1) : 0); break;  // Setfilter
    case 0x02: if (param) xa_stream_setloc(p0, (uint8_t)c->mem_r8(param + 1),     // Setloc
                                           (uint8_t)c->mem_r8(param + 2)); break;
    case 0x06: case 0x1B: xa_stream_start(); break;                           // ReadN / ReadS
    case 0x08: case 0x09: xa_stream_stop(); break;                            // Stop / Pause
    default: break;
  }
  zero_result(c, c->r[A2]); c->r[V0] = 0;
}
// 0x8008A6EC FUN_8008a6ec(noblock, result) CdSync -> 2 (status: complete/ready).
static void ov_cd_sync(Core* c) { zero_result(c, c->r[A1]); c->r[V0] = 2; }

// 0x8001CE90 FUN_8001ce90(cmd, param, result) — the engine's streaming-path CD-command
// wrapper (FUN_8001ce90 -> FUN_8001ce04 -> FUN_80089ce8/FUN_80089b44). Used by the CD
// *streaming* reader FUN_8001cfc8 (task slot 2), which seeks the drive to a target sector
// and then polls the drive position (cmd 0x10 = GetlocL) until the head reaches the target
// window [task+0x54 .. task+0x58]. We serve all CD data synchronously and model NO drive
// motion, so the real FUN_8001ce04 result path leaves the position MSF zeroed -> the position
// (FUN_8008a110 of 00:00:00 = -150) is never in range -> FUN_8001cfc8 busy-spins forever in a
// non-yielding poll, wedging the whole frame (verified: PSXPORT_SPINDBG caught it spinning in
// FUN_8001cfc8 at GetlocL with a0=FFFFFF6A = FUN_8008a110(zeroed MSF)). FIX: report the drive
// AT the requested sector immediately (no seek latency in our model). For GetlocL we fill the
// result buffer with the BCD MSF of the stream's target start LBA (task+0x54), so the head is
// "in range"; FUN_8001cfc8 then proceeds into its normal per-frame *yielding* read loop instead
// of spinning. All other streaming commands report success (our synchronous-CD model). This
// only intercepts the FUN_8001ce90 wrapper — FUN_8001d940's reader calls FUN_8001ce04 directly
// and is unaffected.
static void ov_cd_cmd_stream(Core* c) {
  uint32_t cmd = c->r[A0] & 0xFF, result = c->r[A2];
  if (cfg_dbg("cdcmd")) {
    uint32_t pp = c->r[A1]; uint8_t p[4] = {0,0,0,0};
    if (pp) for (int i = 0; i < 4; i++) p[i] = (uint8_t)c->mem_r8(pp + i);
    fprintf(stderr, "[cdstream] cmd=0x%02X param=[%02X %02X %02X %02X] ra=0x%08X\n",
            cmd, p[0], p[1], p[2], p[3], c->r[31]);
  }
  { uint32_t pp = c->r[A1]; uint8_t p0 = pp ? (uint8_t)c->mem_r8(pp) : 0;
    switch (cmd) {
      case 0x0E: xa_stream_setmode(p0); break;
      case 0x0D: xa_stream_setfilter(p0, pp ? (uint8_t)c->mem_r8(pp + 1) : 0); break;
      case 0x02: if (pp) xa_stream_setloc(p0, (uint8_t)c->mem_r8(pp+1), (uint8_t)c->mem_r8(pp+2)); break;
      case 0x06: case 0x1B: xa_stream_start(); break;
      case 0x08: case 0x09: xa_stream_stop(); break;
      default: break;
    } }
  if (cmd == 0x10 && result) {                  // GetlocL: report the drive-head position.
    // While XA audio is streaming, report the native XA engine's ADVANCING read position so
    // the cutscene's clip-end wait (FUN_8001cfc8: yield while head <= task+0x58) actually
    // terminates — the voice/BGM line plays once, then the scene advances (and pauses us).
    // When not streaming this is a data seek/load: report the target sector (head "arrived",
    // no seek latency in our synchronous-CD model).
    // (XA voice/BGM no longer polls this — it's ported native via FUN_8001d2a8 -> xa_stream_play,
    // see ov_voice_play. This path remains only for any data-streaming GetlocL.)
    uint32_t task = c->mem_r32(0x1f800138);
    uint32_t lba = c->mem_r32(task + 0x54);
    int t = (int)lba + 150;                     // FUN_8008a00c: LBA -> MSF (sector = lba+150)
    int frame = t % 75, rem = t / 75, sec = rem % 60, min = rem / 60;
    c->mem_w8(result + 0, (min % 10) + ((min / 10) << 4));   // BCD min
    c->mem_w8(result + 1, (sec % 10) + ((sec / 10) << 4));   // BCD sec
    c->mem_w8(result + 2, (frame % 10) + ((frame / 10) << 4));// BCD frame
  }
  c->r[V0] = 0;                                 // command succeeded
}

// 0x8008C1EC FUN_8008c1ec(a0=blocks, a1=lba, a2=buf): native synchronous read.
static int  g_cd_verbose = 0;  // PSXPORT_CD_VERBOSE=1
static void ov_cd_read(Core* c) {
  uint32_t blocks = c->r[A0], lba = c->r[A1], buf = c->r[A2];
  uint8_t sec[2048];
  for (uint32_t i = 0; i < blocks; i++) {
    if (!disc_read_sector(lba + i, sec)) { c->r[V0] = 0; return; }  // bool: 0 = failure
    for (uint32_t j = 0; j < 2048; j++) c->mem_w8(buf + i * 2048u + j, sec[j]);
  }
  if (g_cd_verbose)
    fprintf(stderr, "[cd] read %u blk @ LBA %u -> 0x%08X\n", blocks, lba, buf);
  c->r[V0] = 1;  // bool: success
}

// 0x8001DB8C FUN_8001db8c(a0=dest, a1=lba, a2=size_bytes): the engine's file loader. The
// real body spawns a reader sub-task (FUN_8001db38 -> FUN_8001d940) that issues a raw libcd
// ReadN and copies sectors in a per-sector IRQ callback (FUN_8001d7c4, plain CdGetSector copy,
// no decompression) — an async streaming path our no-IRQ overrides can't feed. Replace it
// with a native consecutive-sector read of the same bytes: ceil(size/2048) sectors from `lba`
// into `dest`, copying exactly `size` bytes. Returns param_3 (size), as the original does.
static void ov_cd_loadfile(Core* c) {
  uint32_t dest = c->r[A0], lba = c->r[A1], size = c->r[A2];
  uint8_t sec[2048];
  uint32_t done = 0;
  for (uint32_t i = 0; done < size; i++) {
    if (!disc_read_sector(lba + i, sec)) break;
    uint32_t n = size - done < 2048 ? size - done : 2048;
    for (uint32_t j = 0; j < n; j++) c->mem_w8(dest + done + j, sec[j]);
    done += n;
  }
  if (g_cd_verbose)
    fprintf(stderr, "[cd] loadfile %u B @ LBA %u -> 0x%08X ra=0x%08X\n", size, lba, dest, c->r[31]);
  c->r[V0] = size;
}

// Direct-call native loadfile (used by the PC-native boot path, which owns the START.BIN /
// stage-overlay load top-down instead of dispatching the PSX FUN_8001db8c). Same semantics.
void cd_loadfile_native(Core* c, uint32_t dest, uint32_t lba, uint32_t size) {
  c->r[A0] = dest; c->r[A1] = lba; c->r[A2] = size;
  ov_cd_loadfile(c);
}

// 0x8001D940 FUN_8001d940: the engine's ASYNC streaming reader. It is spawned as task1 (its body
// FUN_8001db38 -> FUN_8001d940) by the area-data loaders FUN_80044cd4 (fire-and-forget) and
// FUN_80044bd4 (spawn + yield-wait), NOT through FUN_8001db8c/FUN_8001dc40 — so the synchronous
// overrides above do not cover it. The real body issues a raw libcd ReadN, then loops (yielding
// each frame) until the remaining word count _DAT_1f8001f4 reaches 0; that count is decremented
// only by the per-sector data-ready IRQ callback FUN_8001d7c4 (a plain CdGetSector copy into
// _DAT_1f8001f8). Our no-IRQ runtime never fires that callback, so the count never hits 0, the
// reader never returns, FUN_8001db38 never sets the load-done flag DAT_1f80019b, and the GAME
// state machine's level-load wait (outer state s48=2 -> 4a=1/4c=2/4e=8 leaf @ FUN_80106f68,
// which polls DAT_1f80019b) spins forever — the level-intro renders once, then the screen stays
// black. FIX (recomp-overrides, mirrors ov_cd_loadfile): do the read natively & synchronously.
// _DAT_1f8001f4 is in 32-bit WORDS (0x200 words = 1 sector = 2048 B); copy words*4 bytes from
// consecutive sectors starting at LBA _DAT_1f8001f0 into _DAT_1f8001f8, exactly word-granular as
// FUN_8001d7c4 does (dest advances by words*4, no sector padding). Then zero the remaining count
// and advance dest/position trackers to the post-read state so FUN_8001d940's caller FUN_8001db38
// (task+0x6c is already 1 = success) sets DAT_1f80019b and ends task1.
void ov_cd_async_read(Core* c) {
  uint32_t lba   = c->mem_r32(0x1f8001f0);
  uint32_t words = c->mem_r32(0x1f8001f4);
  uint32_t dest  = c->mem_r32(0x1f8001f8);
  uint32_t bytes = words * 4u;
  uint8_t sec[2048];
  uint32_t done = 0, nsec = 0;
  for (; done < bytes; nsec++) {
    if (!disc_read_sector(lba + nsec, sec)) break;
    uint32_t n = bytes - done < 2048 ? bytes - done : 2048;
    for (uint32_t j = 0; j < n; j++) c->mem_w8(dest + done + j, sec[j]);
    done += n;
  }
  c->mem_w32(0x1f8001f4, 0);                  // remaining count consumed (callback would zero it)
  c->mem_w32(0x1f8001f8, dest + done);        // dest advanced, as FUN_8001d7c4 leaves it
  if (nsec) c->mem_w32(0x800be0e0, lba + nsec - 1);  // DAT_800be0e0 = last sector read (pos tracker)
  if (g_cd_verbose)
    fprintf(stderr, "[cd] async read %u words (%u B) @ LBA %u -> 0x%08X\n", words, bytes, lba, dest);
}

// Direct-call native FUN_8001DC40(dest, lba, size_bytes): the inline (NON-spawning) sync reader the
// indexed file loaders use (e.g. ov_80045080). FUN_8001DC40 stuffs the scratchpad read descriptor
// (0x1f8001f8=dest, 0x1f8001f0=lba, 0x1f8001f4=ceil(size/4) words) then calls FUN_8001D940 inline; we
// reproduce that by filling the same descriptor and running the synchronous ov_cd_async_read. Used by
// the top-down PC-driven loaders (e.g. DEMO substate s0) so they never enter the IRQ-driven reader.
void cd_dc40_sync(Core* c, uint32_t dest, uint32_t lba, uint32_t size) {
  c->mem_w32(0x1f8001f8, dest);
  c->mem_w32(0x1f8001f0, lba);
  c->mem_w32(0x1f8001f4, (size + 3u) >> 2);   // ceil(size/4) words, as FUN_8001DC40 computes
  ov_cd_async_read(c);
  c->r[V0] = size;                            // FUN_8001DC40 returns size in v0
}

// Platform-HLE entry for FUN_8001DC40 (intercepted for any caller): (a0=dest, a1=lba, a2=size_bytes).
void ov_cd_dc40(Core* c) { cd_dc40_sync(c, c->r[A0], c->r[A1], c->r[A2]); }

// 0x8001D2A8 FUN_8001d2a8(chan, start_lba, end_lba, flags): the engine's voice/BGM clip player.
// It set task-2 fields + spawned the FUN_8001cfc8 streaming-reader coroutine (slot 2) which issued
// the CD commands and busy-polled GetlocL for the clip end; the cutscene then waited
// `while (DAT_801fe0e0 != 0)` (task-2 state). We PORT this engine subsystem to native: play the
// clip directly via xa_stream (no PSX task, no CD-register poll) and own task slot 2 — the native
// scheduler skips the unused coroutine and clears DAT_801fe0e0 when the clip finishes (native_boot).
// All the by-index voice APIs (FUN_8001d71c/d364/d41c/d0e0) funnel through here, so this one
// override covers them. flags bit0 = loop. (Bug it fixes: the recomp coroutine + our scheduler's
// fresh-vs-resume handling mishandled re-registered clips, so a new line never started and the
// cutscene hung on the old clip — the fisherman "AAAGH repeats / scene stuck".)
// ---- dialog-vs-ingame-music coordination (PC mod, instant-CD-safe) ----------------------------
// The ingame/area background music is a LOOPING XA clip; a dialog uses sequenced "dialog-tone"
// songs (current-song byte 0x800bed80 in 4..7 — regular/worry/etc, user-identified) plus
// one-shot voice clips, all on the single XA stream. On real hardware the area-music start fires
// from the gameplay state machine only AFTER the CD-paced scene load, by which time the dialog
// has the stream/gate, so the looping music never overlapped the dialog tone. With our instant
// CD reads the area-music start fires immediately (during the dialog gap), so the loop overlaps
// the dialog tone — the audible bug. Mod: while a dialog tone is the current song, keep the
// looping ingame music suppressed and remembered; resume it once the dialog ends. One-shot voice
// clips are unaffected (they ARE the dialog audio).
// deferred-music state now lives on the instance: c->game->cd.{pending_music,pm_chan,pm_start,pm_end}
static int dialog_tone_active(Core* c) {
  uint32_t s = c->mem_r16(0x800bed80) & 0xFFFF;
  return s >= 4 && s <= 7;
}
// Enable CD->SPU mixing (libsnd SpuSetCommonAttr via FUN_8001cf00(1)); needed for the SPU to
// actually mix the decoded XA (Beetle spu.c gates on SPUControl bit0).
static void cd_to_spu_mix(Core* c, int on) { c->r[A0] = on ? 1 : 0; rec_dispatch(c, 0x8001cf00u); }

// Fade the ingame music IN from silence using the GAME'S OWN CD-volume ramp. The game keeps a
// CD-volume fade pair: target DAT_800be222 and current DAT_800be224 (the SpuCommonAttr CDVOL the
// per-frame audio update FUN_80075824 ramps current->target by 0x100/frame, then writes to SPU
// reg 0x1B0/0x1B2, which Beetle spu.c uses to scale the XA — see spu.c CDVol). At steady state
// both sit at 0x7fff (full). On real hardware the area music only started after the CD-paced
// scene load, by which point this fade had been (re)armed; with instant CD the music (re)starts
// at the steady full level — the "starts too loud" bug. Mod: snap the fade CURRENT to 0 (leave the
// target), so the game's own ramp climbs it back up = a ~2s fade-in. Caller MUST only do this when
// the music is the sole XA user — CDVOL also scales the dialog VOICE (chan22), so zeroing it while
// a voice plays would mute the voice too.
//
// Snapping only `cur` is NOT enough: the game's per-frame ramp FUN_80075824 writes the SPU CDVOL
// register (0x1F801DB0/DB2 — Beetle spu.c case 0x30/0x32, CDVol[0]/[1]) from `cur` ONCE per frame,
// and on the music-(re)start frame it already ran with the OLD (full) `cur` before this snap — so
// that one frame still mixes the freshly-started XA at full volume (verified: PSXPORT_XA_DBG showed
// CDVOL=25480 for the start frame, then 198 and climbing). That leading full-volume frame is the
// audible "1-frame blip" / "starts loud then drops to zero then climbs". So ALSO write the SPU CDVOL
// register to 0 directly here, muting the start frame's mix; the next frame the ramp rewrites it from
// cur=0 and climbs. mem_w16 to 0x1F801DBx routes through io_write -> spu_write -> SPU_Write (mem.c).
static void music_fade_in(Core* c) {
  c->mem_w16(0x800be224, 0);   // game's fade CURRENT = 0 (its per-frame ramp climbs it back up)
  c->mem_w16(0x1f801db0, 0);   // SPU CDVOL L = 0 NOW, so THIS frame's XA mix is silent (no loud blip)
  c->mem_w16(0x1f801db2, 0);   // SPU CDVOL R = 0
}

// Diagnostic: trace the game's CD-volume fade state + XA stream lifecycle, on change only.
// tgt/cur = DAT_800be222/224 (fade target/current), mas = DAT_800be220 (master),
// 19a/137 = state bytes gating FUN_80075824's ramp, song = 0x800bed80, gate = 0x801fe0e0.
extern volatile uint32_t g_bgm_frame;
void xa_audio_trace(Core* c, const char* tag) {
  if (!cfg_str("PSXPORT_XA_DBG")) return;
  static int t=1<<30,cur,mas,s19a,s137,song,act,lp,gate;
  int nt=(int16_t)c->mem_r16(0x800be222), ncur=(int16_t)c->mem_r16(0x800be224), nmas=(int16_t)c->mem_r16(0x800be220);
  int n19a=c->mem_r8(0x1f80019a), n137=c->mem_r8(0x1f800137);
  int nsong=c->mem_r16(0x800bed80)&0xffff, nact=xa_stream_is_active(), nlp=xa_stream_is_looping();
  int ngate=c->mem_r16(0x801fe0e0)&0xffff;
  if (nt!=t||ncur!=cur||nmas!=mas||n19a!=s19a||n137!=s137||nsong!=song||nact!=act||nlp!=lp||ngate!=gate) {
    fprintf(stderr,"[xa f%u %-5s] tgt=%d cur=%d mas=%d 19a=%d 137=%d song=%d act=%d loop=%d gate=%d\n",
            g_bgm_frame,tag,nt,ncur,nmas,n19a,n137,nsong,nact,nlp,ngate);
    t=nt;cur=ncur;mas=nmas;s19a=n19a;s137=n137;song=nsong;act=nact;lp=nlp;gate=ngate;
  }
}

static void ov_voice_play(Core* c) {
  uint8_t  chan  = (uint8_t)(c->r[A0] & 0xFF);
  uint32_t start = c->r[A1], end = c->r[A2];
  int      loop  = (int)(c->r[7] & 1);              // a3 = flags
  if (cfg_str("PSXPORT_XA_DBG"))
    fprintf(stderr, "[voice_play] chan=%u [%u..%u] loop=%d ra=%08X\n", chan, start, end, loop, c->r[31]);
  if (loop) {                                       // looping clip == ingame/area background music
    c->game->cd.pending_music = 1; c->game->cd.pm_chan = chan; c->game->cd.pm_start = start; c->game->cd.pm_end = end;
    if (dialog_tone_active(c)) return;               // suppress during a dialog; resumed by xa_dialog_coord
  }
  xa_stream_play(chan, start, end, loop);
  c->mem_w16(0x801fe0e0, 2);                            // task-2 state = running (cutscene wait gate)
  cd_to_spu_mix(c, 1);
  if (loop) music_fade_in(c);                         // ingame music fades in from 0 (instant-CD mod)
}

// Called from the BGM-start override (ov_bgm_start) right after the song index is written, i.e. at
// the instant a dialog tone begins — synchronously, before this frame's audio mix. Stops the looping
// ingame music so it can't leak a frame past the dialog start (the per-frame xa_dialog_coord below
// would otherwise stop it one frame late, after the mix). Keeps it remembered (s_pending_music) so
// xa_dialog_coord resumes it when the dialog ends.
void xa_music_cut_if_dialog(Core* c) {
  if (dialog_tone_active(c) && xa_stream_is_looping()) xa_stream_stop();
}

// Called once per frame (native_scheduler_step). Enforces "dialogs stop the ingame music":
// stop a looping ingame-music clip while a dialog tone is up; resume the remembered clip once
// the dialog ends and the XA stream is free (no voice playing).
void xa_dialog_coord(Core* c) {
  if (cfg_str("PSXPORT_XA_DBG")) {
    static uint32_t prev = 0xDEAD; static int pa = -1, pl = -1;
    uint32_t s = c->mem_r16(0x800bed80) & 0xFFFF; int a = xa_stream_is_active(), l = xa_stream_is_looping();
    if (s != prev || a != pa || l != pl) {
      fprintf(stderr, "[coord] song=%u tone=%d xa_active=%d loop=%d pending=%d\n",
              s, dialog_tone_active(c), a, l, c->game->cd.pending_music);
      prev = s; pa = a; pl = l;
    }
  }
  if (dialog_tone_active(c)) {
    if (xa_stream_is_looping()) xa_stream_stop();    // dialog up: silence ingame music (kept pending)
  } else if (c->game->cd.pending_music && !xa_stream_is_active()) {
    xa_stream_play(c->game->cd.pm_chan, c->game->cd.pm_start, c->game->cd.pm_end, 1);   // dialog over: resume ingame music
    c->mem_w16(0x801fe0e0, 2);
    cd_to_spu_mix(c, 1);
    music_fade_in(c);                                      // resumed music fades in from 0 (no voice now)
  }
}
// 0x8001CF2C FUN_8001cf2c: stop the current voice/BGM clip.
static void ov_voice_stop(Core* c) {
  xa_stream_stop(); c->mem_w16(0x801fe0e0, 0);
  c->r[A0] = 0; rec_dispatch(c, 0x8001cf00u);        // CD->SPU mix off
}

// ===========================================================================================
// NATIVE HLE CD — boot init (replaces the PSX libcd CdInit at FUN_800898a0 / FUN_80089930 /
// FUN_8008b2d8). The PC port models NO CD controller: every CD operation is a native synchronous
// call that resolves as fast as the host can (data reads via disc_read_sector / cd_loadfile_native,
// command/sync via the ov_cd_* bodies above). There must be NO busy-wait anywhere.
//
// The recomp libcd init busy-waits: FUN_800898a0 retries FUN_80089930 (CdInit) up to 5 times, and
// each CdInit calls the low-level reset FUN_8008b2d8, which pokes the CD HW registers (0x1F801800
// region — unmodelled) then spins in CD_cw on the controller-ready bit DAT_800ac298/299, which no
// IRQ ever sets → "CD timeout" → "CdInit: Init failed". None of that HW state is read by our native
// CD path (cd_loadfile_native / disc_find_file / the ov_cd_* HLE), so we skip the entire handshake
// and just leave RAM in the state FUN_800898a0's SUCCESS path leaves it: the four CD-event callback
// pointers installed (matching the proven-good path where the low-level reset returned ready). The
// callbacks are dead in our model (no IRQ invokes them; every command completes inline), but we
// install them so any code that inspects the table sees the same values as on real hardware.
void cd_hle_init(Core* c) {
  // FUN_800898a0 success path (0x800898c4..0x800898fc): install the CD-event callback table.
  c->mem_w32(0x800abfbcu, 0x8009996cu);   // CD-ready / sync callback
  c->mem_w32(0x800abfc0u, 0x80089994u);   // CD-ready-cb 2
  c->mem_w32(0x800abf24u, 0x800899bcu);   // CD event handler
  c->mem_w32(0x800abf28u, 0x00000000u);   // (cleared)
  if (g_cd_verbose || cfg_dbg("cd"))
    fprintf(stderr, "[cd] HLE CdInit: drive ready (no controller, no handshake, no busy-wait)\n");
}

void cd_overrides_init(void) {
  if (cfg_dbg("cd")) g_cd_verbose = 1;
  // SYNC the inline async CD loader FUN_8001DC40(dest, lba, size_bytes): it stuffs the scratchpad read
  // descriptor then runs the IRQ-driven reader FUN_8001D940 inline, which (no IRQ in our model) never
  // drains the word count and hits CD_cw -> VSync (now trapped). Replace the whole entry with the native
  // synchronous read (cd_dc40_sync sets the same descriptor + reads the sectors straight off the disc).
  // NB intercept THIS entry (clean a0/a1/a2 contract), NOT the shared core FUN_8001D940 — that core is
  // also driven by the COOPERATIVE area-load task (FUN_80044bd4 -> task-1), whose descriptor setup differs;
  // force-syncing the core corrupted the area overlay. (User 2026-06-22: async CD read -> do it sync.)
  void platform_hle_register(uint32_t, OverrideFn);
  void ov_cd_dc40(Core*);
  platform_hle_register(0x8001DC40u, ov_cd_dc40);   // FUN_8001dc40 inline async loader -> sync
  // 0x8001DC40 FUN_8001dc40(a0=dest, a1=lba, a2=size_bytes): the intro sequencer's loader
  // variant. Same (dest, lba, size_bytes) contract as FUN_8001db8c — it sets the identical
  // _DAT_1f8001f8/f0/f4 read state — but runs the reader INLINE (calls FUN_8001d940 directly,
  // no spawned sub-task / no DAT_801fe070 busy-wait guard). The real body falls into the same
  // async/IRQ ReadN path (FUN_8001d940) that spins on _DAT_1f8001f4, decremented only by the
  // un-fired per-sector IRQ callback FUN_8001d7c4 — so the intro loader coroutine
  // (FUN_80044f58/FUN_8004514c) stalls forever. The inline variant has no guard to clear, so
  // the same native synchronous read body applies verbatim: copy `size` bytes from `lba` into
  // `dest`, return size. Callers set their own done-flag (DAT_1f80019b) after the call.
}

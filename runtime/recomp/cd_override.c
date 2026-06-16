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
#include "r3000.h"
#include <stdio.h>
#include <stdlib.h>

enum { V0 = 2, A0 = 4, A1 = 5, A2 = 6 };

int disc_read_sector(uint32_t lba, uint8_t* out);

// Native in-game XA-ADPCM streaming (xa_stream.c). The CdControl wrapper below feeds it the
// streaming commands the game uses for cutscene BGM / voice (Setmode XA bit, Setloc, ReadS).
void xa_stream_setmode(uint8_t mode);
void xa_stream_setfilter(uint8_t file, uint8_t chan);
void xa_stream_setloc(uint8_t amm, uint8_t ass, uint8_t asect);
void xa_stream_start(void);
void xa_stream_stop(void);
int  xa_stream_play_lba(uint32_t* lba);
void xa_stream_play(uint8_t chan, uint32_t start, uint32_t end, int loop);

// 0x8008B2D8 FUN_8008b2d8: low-level CdInit -> success (drive ready), no HW handshake.
static void ov_cdinit(R3000* c) { c->r[V0] = 0; }

// libcd command/sync primitives whose real bodies spin in a CD_cw / CD timeout loop on the
// IRQ-set status DAT_800ac298 (never set — no controller). Since every DATA read is served
// natively by ov_cd_read, the remaining CdControl/CdSync calls (Setmode, Pause, Nop, ...)
// only need to report success so their waits fall through. We replace, not super-call: the
// real bodies cannot return without the IRQ. Result bytes (drive status) the caller may copy
// are zeroed — callers on the boot path branch on the return value, not the status bytes.
static void zero_result(uint32_t p) { if (p) for (int i = 0; i < 8; i++) mem_w8(p + i, 0); }

// 0x8008AC34 FUN_8008ac34(cmd, param, result, mode) CdCommand -> 0 (success).
static void ov_cd_command(R3000* c) {
  if (getenv("PSXPORT_CDCMD_DBG")) {
    uint32_t cmd = c->r[A0] & 0xFF, param = c->r[A1];
    uint8_t p[4] = {0,0,0,0};
    if (param) for (int i = 0; i < 4; i++) p[i] = (uint8_t)mem_r8(param + i);
    fprintf(stderr, "[cdcmd] cmd=0x%02X param=[%02X %02X %02X %02X] mode=%u ra=0x%08X\n",
            cmd, p[0], p[1], p[2], p[3], c->r[7], c->r[31]);
  }
  // In-game XA-ADPCM streaming: the game drives cutscene BGM / voice through these controller
  // commands. We don't model the controller (data is read natively elsewhere), so route the
  // streaming-relevant ones to the native XA engine; everything else still just ACKs.
  uint32_t cmd = c->r[A0] & 0xFF, param = c->r[A1];
  uint8_t p0 = param ? (uint8_t)mem_r8(param) : 0;
  switch (cmd) {
    case 0x0E: xa_stream_setmode(p0); break;                                  // Setmode
    case 0x0D: xa_stream_setfilter(p0, param ? (uint8_t)mem_r8(param + 1) : 0); break;  // Setfilter
    case 0x02: if (param) xa_stream_setloc(p0, (uint8_t)mem_r8(param + 1),     // Setloc
                                           (uint8_t)mem_r8(param + 2)); break;
    case 0x06: case 0x1B: xa_stream_start(); break;                           // ReadN / ReadS
    case 0x08: case 0x09: xa_stream_stop(); break;                            // Stop / Pause
    default: break;
  }
  zero_result(c->r[A2]); c->r[V0] = 0;
}
// 0x8008A6EC FUN_8008a6ec(noblock, result) CdSync -> 2 (status: complete/ready).
static void ov_cd_sync(R3000* c) { zero_result(c->r[A1]); c->r[V0] = 2; }

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
static void ov_cd_cmd_stream(R3000* c) {
  uint32_t cmd = c->r[A0] & 0xFF, result = c->r[A2];
  if (getenv("PSXPORT_CDCMD_DBG")) {
    uint32_t pp = c->r[A1]; uint8_t p[4] = {0,0,0,0};
    if (pp) for (int i = 0; i < 4; i++) p[i] = (uint8_t)mem_r8(pp + i);
    fprintf(stderr, "[cdstream] cmd=0x%02X param=[%02X %02X %02X %02X] ra=0x%08X\n",
            cmd, p[0], p[1], p[2], p[3], c->r[31]);
  }
  { uint32_t pp = c->r[A1]; uint8_t p0 = pp ? (uint8_t)mem_r8(pp) : 0;
    switch (cmd) {
      case 0x0E: xa_stream_setmode(p0); break;
      case 0x0D: xa_stream_setfilter(p0, pp ? (uint8_t)mem_r8(pp + 1) : 0); break;
      case 0x02: if (pp) xa_stream_setloc(p0, (uint8_t)mem_r8(pp+1), (uint8_t)mem_r8(pp+2)); break;
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
    uint32_t task = mem_r32(0x1f800138);
    uint32_t lba = mem_r32(task + 0x54);
    int t = (int)lba + 150;                     // FUN_8008a00c: LBA -> MSF (sector = lba+150)
    int frame = t % 75, rem = t / 75, sec = rem % 60, min = rem / 60;
    mem_w8(result + 0, (min % 10) + ((min / 10) << 4));   // BCD min
    mem_w8(result + 1, (sec % 10) + ((sec / 10) << 4));   // BCD sec
    mem_w8(result + 2, (frame % 10) + ((frame / 10) << 4));// BCD frame
  }
  c->r[V0] = 0;                                 // command succeeded
}

// 0x8008C1EC FUN_8008c1ec(a0=blocks, a1=lba, a2=buf): native synchronous read.
static int  g_cd_verbose = 0;  // PSXPORT_CD_VERBOSE=1
static void ov_cd_read(R3000* c) {
  uint32_t blocks = c->r[A0], lba = c->r[A1], buf = c->r[A2];
  uint8_t sec[2048];
  for (uint32_t i = 0; i < blocks; i++) {
    if (!disc_read_sector(lba + i, sec)) { c->r[V0] = 0; return; }  // bool: 0 = failure
    for (uint32_t j = 0; j < 2048; j++) mem_w8(buf + i * 2048u + j, sec[j]);
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
static void ov_cd_loadfile(R3000* c) {
  uint32_t dest = c->r[A0], lba = c->r[A1], size = c->r[A2];
  uint8_t sec[2048];
  uint32_t done = 0;
  for (uint32_t i = 0; done < size; i++) {
    if (!disc_read_sector(lba + i, sec)) break;
    uint32_t n = size - done < 2048 ? size - done : 2048;
    for (uint32_t j = 0; j < n; j++) mem_w8(dest + done + j, sec[j]);
    done += n;
  }
  if (g_cd_verbose)
    fprintf(stderr, "[cd] loadfile %u B @ LBA %u -> 0x%08X ra=0x%08X\n", size, lba, dest, c->r[31]);
  c->r[V0] = size;
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
static void ov_cd_async_read(R3000* c) {
  (void)c;
  uint32_t lba   = mem_r32(0x1f8001f0);
  uint32_t words = mem_r32(0x1f8001f4);
  uint32_t dest  = mem_r32(0x1f8001f8);
  uint32_t bytes = words * 4u;
  uint8_t sec[2048];
  uint32_t done = 0, nsec = 0;
  for (; done < bytes; nsec++) {
    if (!disc_read_sector(lba + nsec, sec)) break;
    uint32_t n = bytes - done < 2048 ? bytes - done : 2048;
    for (uint32_t j = 0; j < n; j++) mem_w8(dest + done + j, sec[j]);
    done += n;
  }
  mem_w32(0x1f8001f4, 0);                  // remaining count consumed (callback would zero it)
  mem_w32(0x1f8001f8, dest + done);        // dest advanced, as FUN_8001d7c4 leaves it
  if (nsec) mem_w32(0x800be0e0, lba + nsec - 1);  // DAT_800be0e0 = last sector read (pos tracker)
  if (g_cd_verbose)
    fprintf(stderr, "[cd] async read %u words (%u B) @ LBA %u -> 0x%08X\n", words, bytes, lba, dest);
}

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
static void ov_voice_play(R3000* c) {
  uint8_t  chan  = (uint8_t)(c->r[A0] & 0xFF);
  uint32_t start = c->r[A1], end = c->r[A2];
  int      loop  = (int)(c->r[7] & 1);              // a3 = flags
  xa_stream_play(chan, start, end, loop);
  mem_w16(0x801fe0e0, 2);                            // task-2 state = running (cutscene wait gate)
}
// 0x8001CF2C FUN_8001cf2c: stop the current voice/BGM clip.
static void ov_voice_stop(R3000* c) {
  (void)c; xa_stream_stop(); mem_w16(0x801fe0e0, 0);
}

void cd_overrides_init(void) {
  if (getenv("PSXPORT_CD_VERBOSE")) g_cd_verbose = 1;
  rec_set_override(0x8001D2A8u, ov_voice_play);      // engine voice/BGM clip player -> native xa_stream
  rec_set_override(0x8001CF2Cu, ov_voice_stop);      // stop voice/BGM -> native
  rec_set_override(0x8001D940u, ov_cd_async_read);   // engine async streaming reader (task1)
  rec_set_override(0x8008B2D8u, ov_cdinit);
  rec_set_override(0x8001DB8Cu, ov_cd_loadfile);
  // 0x8001DC40 FUN_8001dc40(a0=dest, a1=lba, a2=size_bytes): the intro sequencer's loader
  // variant. Same (dest, lba, size_bytes) contract as FUN_8001db8c — it sets the identical
  // _DAT_1f8001f8/f0/f4 read state — but runs the reader INLINE (calls FUN_8001d940 directly,
  // no spawned sub-task / no DAT_801fe070 busy-wait guard). The real body falls into the same
  // async/IRQ ReadN path (FUN_8001d940) that spins on _DAT_1f8001f4, decremented only by the
  // un-fired per-sector IRQ callback FUN_8001d7c4 — so the intro loader coroutine
  // (FUN_80044f58/FUN_8004514c) stalls forever. The inline variant has no guard to clear, so
  // the same native synchronous read body applies verbatim: copy `size` bytes from `lba` into
  // `dest`, return size. Callers set their own done-flag (DAT_1f80019b) after the call.
  rec_set_override(0x8001DC40u, ov_cd_loadfile);
  rec_set_override(0x8008AC34u, ov_cd_command);
  rec_set_override(0x8008A6ECu, ov_cd_sync);
  rec_set_override(0x8001CE90u, ov_cd_cmd_stream);   // streaming CD-cmd wrapper (GetlocL pos)
  rec_set_override(0x8008C1ECu, ov_cd_read);
}

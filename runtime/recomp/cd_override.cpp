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
#include "c_subsys.h"
#include "core.h"
#include "game.h"
#include "overlay_router.h"
#include "platform_hle.h" // class PlatformHle — CD-subsystem HLE registrations go through the singleton
#include <chrono>
#include <lucent/log.h>
#include <stdio.h>
#include <stdlib.h>

enum { V0 = 2, A0 = 4, A1 = 5, A2 = 6 };

// Native in-game XA-ADPCM streaming (xa_stream.c). The CdControl wrapper below feeds it the
// streaming commands the game uses for cutscene BGM / voice (Setmode XA bit, Setloc, ReadS).

// 0x8008B2D8 FUN_8008b2d8: low-level CdInit -> success (drive ready), no HW handshake.
static void cdinit(Core *c) {
  c->r[V0] = 0;
}

// libcd command/sync primitives whose real bodies spin in a CD_cw / CD timeout loop on the
// IRQ-set status DAT_800ac298 (never set — no controller). Since every DATA read is served
// natively by cd_read, the remaining CdControl/CdSync calls (Setmode, Pause, Nop, ...)
// only need to report success so their waits fall through. We replace, not super-call: the
// real bodies cannot return without the IRQ. Result bytes (drive status) the caller may copy
// are zeroed — callers on the boot path branch on the return value, not the status bytes.
static void zero_result(Core *c, uint32_t p) {
  if (p) {
    for (int i = 0; i < 8; i++) {
      c->mem_w8(p + i, 0);
    }
  }
}

// 0x8008AC34 FUN_8008ac34(cmd, param, result, mode) CdCommand -> 0 (success).
// Drive a STOCK-libcd read to completion, natively, with no interrupt.
//
// A stock-libcd read is a per-sector CALLBACK LOOP: the game installs a ready callback with
// CdReadyCallback(), issues ReadN, and expects that callback to be invoked once per sector. Its body
// fetches the sector (CdGetSector), advances its own destination pointer, decrements its remaining
// count, and when the count reaches zero restores the callbacks and issues Pause.
//
// So the port does not need an interrupt controller, a CD IRQ, or the guest's ISR chain to finish a
// read. It needs to call the callback the game already registered. That is the whole mechanism.
//
// TERMINATION is the guest's own signal, not a guess: its callback issues Pause/Stop through this
// same cd_command, which clears `reading`. The loop is additionally bounded, and hitting the bound
// is reported LOUDLY rather than silently truncating a read — a short read that looks successful is
// exactly the failure this layer must never produce.
static void cd_drive_stock_read(Core *c) {
  const GameConfig *cfg = c->cfg;
  if (!cfg || !cfg->cdReadyCbPtr) {
    return; // not a stock-libcd game, or not RE'd yet
  }

  Cd &cd = c->game->cd;
  if (cd.in_stock_read) {
    return; // the callback re-issued a read; let the outer loop run
  }
  cd.in_stock_read = 1;
  cd.stock_reading = 1;

  // The guest callback runs as a normal function: save and restore the whole register context around
  // it, exactly as an exception entry would, so the interrupted caller sees nothing.
  const R3000 saved = *static_cast<R3000 *>(c);

  enum { kMaxSectors = 65536 }; // ~150 MB; larger than any single PSX read
  unsigned n = 0;
  for (; n < kMaxSectors && cd.stock_reading; n++) {
    const uint32_t cb = c->mem_r32(cfg->cdReadyCbPtr);
    if (!cb) {
      break; // no callback installed -> nothing to drive
    }
    c->r[A0] = 1; // libcd passes the completion status as arg 1
    c->r[A1] = 0;
    rec_dispatch(c, cb);
  }

  *static_cast<R3000 *>(c) = saved;
  cd.in_stock_read = 0;
  if (n >= kMaxSectors) {
    lucent::error("cd",
                  "stock read did not terminate after {} sectors — the guest never issued "
                  "Pause/Stop. Read ABANDONED; treat any data from it as incomplete.",
                  n);
  } else if (cd.verbose || lucent::channel_on("cd")) {
    lucent::info("cd", "stock read complete: {} ready-callback invocation(s)", n);
  }
}

static void cd_command(Core *c) {
  if (lucent::channel_on("cdcmd")) {
    uint32_t cmd = c->r[A0] & 0xFF, param = c->r[A1];
    uint8_t p[4] = {0, 0, 0, 0};
    if (param) {
      for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)c->mem_r8(param + i);
      }
    }
    lucent::debug("cdcmd",
                  "cmd=0x{:02X} param=[{:02X} {:02X} {:02X} {:02X}] mode={} ra=0x{:08X}",
                  cmd,
                  p[0],
                  p[1],
                  p[2],
                  p[3],
                  c->r[7],
                  c->r[31]);
  }
  // In-game XA-ADPCM streaming: the game drives cutscene BGM / voice through these controller
  // commands. We don't model the controller (data is read natively elsewhere), so route the
  // streaming-relevant ones to the native XA engine; everything else still just ACKs.
  uint32_t cmd = c->r[A0] & 0xFF, param = c->r[A1];
  uint8_t p0 = param ? (uint8_t)c->mem_r8(param) : 0;
  // Inherit the bookkeeping the replaced routine performed. Stock libcd's command-send records the
  // Setloc parameter and the Setmode byte in guest RAM, and its own read path reads them back later
  // (CdPosToInt(CdLastPos()) seeds the expected-sector counter). An override that acknowledges the
  // command without maintaining that state leaves the guest reasoning from stale bytes.
  if (const uint32_t lp = c->cfg ? c->cfg->cdLastPosBuf : 0) {
    if (cmd == 0x02 && param) { // Setloc: the 4-byte position parameter
      for (uint32_t i = 0; i < 4; i++) {
        c->mem_w8(lp + i, c->mem_r8(param + i));
      }
    } else if (cmd == 0x0E && param) { // Setmode: the mode byte, stored just after the position
      c->mem_w8(lp + 4, p0);
    }
  }
  switch (cmd) {
  case 0x0E: // Setmode
    xa_stream_setmode(&c->game->xa, p0);
    // Mirror it into the CONTROLLER as well. Bit 0x20 selects whole-sector framing, and a
    // streaming reader reads the first 8 words of each sector as header+subheader to identify it.
    // Forwarding only to the XA streamer left the controller in user-data framing, so those reads
    // returned picture bytes and the reader re-requested the same sector indefinitely.
    cdc_set_mode(&c->game->cdc, p0);
    break;
  case 0x0D:
    xa_stream_setfilter(&c->game->xa, p0, param ? (uint8_t)c->mem_r8(param + 1) : 0);
    break; // Setfilter
  case 0x02:
    if (param) { // Setloc
      const uint8_t mm = p0, ss = (uint8_t)c->mem_r8(param + 1), ff = (uint8_t)c->mem_r8(param + 2);
      xa_stream_setloc(&c->game->xa, mm, ss, ff);
      // ALSO remember it as a DATA read position. The XA streamer above is the audio path; a game on
      // stock libcd positions the drive here and then issues a read that carries no LBA argument, so
      // this is the only place that target sector is ever stated. Pure bookkeeping — see Cd::setloc_lba.
      // Repositioning the drive INVALIDATES whatever sector is buffered. Without this the cursor
      // keeps popping the previously loaded sector, so the next header read returns mid-sector user
      // data and the guest's drive-position check compares against garbage.
      c->game->cd.sec_pos = 0;
      c->game->cd.sec_len = 0;
      c->game->cd.sec_lba = -1;
      auto bcd = [](uint8_t v) {
        return (v >> 4) * 10 + (v & 0x0F);
      };
      const int lba = (bcd(mm) * 60 + bcd(ss)) * 75 + bcd(ff) - 150; // MSF -> LBA (sector 0 == 00:02:00)
      c->game->cd.setloc_lba = lba >= 0 ? lba : -1;
      if (c->game->cd.verbose) {
        lucent::info("cd", "setloc {:02X}:{:02X}:{:02X} -> LBA {}", mm, ss, ff, c->game->cd.setloc_lba);
      }
    }
    break;
  case 0x06:
  case 0x1B: // ReadN / ReadS
    xa_stream_start(&c->game->xa);
    // Position and load the CONTROLLER too. Streaming code bypasses libcd and waits on the CD
    // status DRQSTS bit before kicking DMA3; with only the native path served, that bit never set
    // and the streaming poller spun forever. Both layers now read the same disc image.
    if (c->game->cd.setloc_lba >= 0) {
      cdc_begin_read(&c->game->cdc, (uint32_t)c->game->cd.setloc_lba);
    }
    // A ReadN reaching this handler is a CONTINUOUS read. File reads no longer arrive here at all:
    // CdRead is served natively above, so the guest's finite read state machine never issues one.
    // That makes this a clean discriminator rather than a guess — mark the stream and let the
    // periodic pump drive it, instead of the self-terminating burst a file read wants.
    c->game->cd.stream_active = 1;
    // Fresh pacing budget per stream, so a new movie cannot inherit the previous one's credit and
    // burst its opening sectors.
    c->game->cd.stream_t0_ns = 0;
    c->game->cd.stream_delivered = 0;
    // Run the file-read burst ONLY for a game that has not taken over CdRead. Where CdRead is
    // served natively (cdReadStock set), a finite read never issues ReadN, so every ReadN arriving
    // here is a CONTINUOUS read — which has no end for the burst to reach. It ran away to its
    // 65536-sector bound and wedged the boot, doing the streaming reader's job badly instead of
    // letting it drive itself through the controller.
    if (!c->cfg || !c->cfg->cdReadStock) {
      cd_drive_stock_read(c);
    }
    // Deliberately NOT running the file-read burst otherwise. That burst drives a finite
    // read's per-sector callback to completion, and it was correct while CdRead ran on the
    // substrate. CdRead is now served natively, so a finite read never issues ReadN — every ReadN
    // reaching this handler is a CONTINUOUS read, which has no end for the burst to reach. It ran
    break;
  case 0x08:
  case 0x09: // Stop / Pause
    xa_stream_stop(&c->game->xa);
    c->game->cd.stock_reading = 0; // the guest's own end-of-read signal; ends cd_drive_stock_read
    c->game->cd.stream_active = 0; // ...and ends the continuous-read pump. The guest decides.
    break;
  default:
    break;
  }
  zero_result(c, c->r[A2]);
  c->r[V0] = 0;
}

// Report blocking-control success after applying the synchronous command
// effects.
void cd_control_sync(Core *c) {
  cd_command(c);
  c->r[V0] = 1;
}
// 0x8008A6EC FUN_8008a6ec(noblock, result) CdSync -> 2 (status: complete/ready).
static void cd_sync(Core *c) {
  zero_result(c, c->r[A1]);
  c->r[V0] = 2;
}

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
static void cd_cmd_stream(Core *c) {
  uint32_t cmd = c->r[A0] & 0xFF, result = c->r[A2];
  if (lucent::channel_on("cdcmd")) {
    uint32_t pp = c->r[A1];
    uint8_t p[4] = {0, 0, 0, 0};
    if (pp) {
      for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)c->mem_r8(pp + i);
      }
    }
    lucent::debug("cdcmd",
                  "[cdstream] cmd=0x{:02X} param=[{:02X} {:02X} {:02X} {:02X}] ra=0x{:08X}",
                  cmd,
                  p[0],
                  p[1],
                  p[2],
                  p[3],
                  c->r[31]);
  }
  {
    uint32_t pp = c->r[A1];
    uint8_t p0 = pp ? (uint8_t)c->mem_r8(pp) : 0;
    switch (cmd) {
    case 0x0E:
      xa_stream_setmode(&c->game->xa, p0);
      cdc_set_mode(&c->game->cdc, p0);
      break;
    case 0x0D:
      xa_stream_setfilter(&c->game->xa, p0, pp ? (uint8_t)c->mem_r8(pp + 1) : 0);
      break;
    case 0x02:
      if (pp) {
        xa_stream_setloc(&c->game->xa, p0, (uint8_t)c->mem_r8(pp + 1), (uint8_t)c->mem_r8(pp + 2));
      }
      break;
    case 0x06:
    case 0x1B:
      xa_stream_start(&c->game->xa);
      break;
    case 0x08:
    case 0x09:
      xa_stream_stop(&c->game->xa);
      break;
    default:
      break;
    }
  }
  if (cmd == 0x10 && result) { // GetlocL: report the drive-head position.
    // While XA audio is streaming, report the native XA engine's ADVANCING read position so
    // the cutscene's clip-end wait (FUN_8001cfc8: yield while head <= task+0x58) actually
    // terminates — the voice/BGM line plays once, then the scene advances (and pauses us).
    // When not streaming this is a data seek/load: report the target sector (head "arrived",
    // no seek latency in our synchronous-CD model).
    // (XA voice/BGM no longer polls this — it's ported native via FUN_8001d2a8 -> xa_stream_play,
    // see voice_play. This path remains only for any data-streaming GetlocL.)
    uint32_t task = c->mem_r32(0x1f800138);
    uint32_t lba = c->mem_r32(task + 0x54);
    int t = (int)lba + 150; // FUN_8008a00c: LBA -> MSF (sector = lba+150)
    int frame = t % 75, rem = t / 75, sec = rem % 60, min = rem / 60;
    c->mem_w8(result + 0, (min % 10) + ((min / 10) << 4));     // BCD min
    c->mem_w8(result + 1, (sec % 10) + ((sec / 10) << 4));     // BCD sec
    c->mem_w8(result + 2, (frame % 10) + ((frame / 10) << 4)); // BCD frame
  }
  c->r[V0] = 0; // command succeeded
}

// 0x8008C1EC FUN_8008c1ec(a0=blocks, a1=lba, a2=buf): native synchronous read.
static void cd_read(Core *c) {
  uint32_t blocks = c->r[A0], lba = c->r[A1], buf = c->r[A2];
  uint8_t sec[2048];
  for (uint32_t i = 0; i < blocks; i++) {
    if (!disc_read_sector(&c->game->disc, lba + i, sec)) {
      c->r[V0] = 0;
      return;
    } // bool: 0 = failure
    for (uint32_t j = 0; j < 2048; j++) {
      c->mem_w8(buf + i * 2048u + j, sec[j]);
    }
  }
  if (c->game->cd.verbose) {
    lucent::info("cd", "read {} blk @ LBA {} -> 0x{:08X}", blocks, lba, buf);
  }
  c->r[V0] = 1; // bool: success
}

// 0x8001DB8C FUN_8001db8c(a0=dest, a1=lba, a2=size_bytes): the engine's file loader. The
// real body spawns a reader sub-task (FUN_8001db38 -> FUN_8001d940) that issues a raw libcd
// ReadN and copies sectors in a per-sector IRQ callback (FUN_8001d7c4, plain CdGetSector copy,
// no decompression) — an async streaming path our no-IRQ overrides can't feed. Replace it
// with a native consecutive-sector read of the same bytes: ceil(size/2048) sectors from `lba`
// into `dest`, copying exactly `size` bytes. Returns param_3 (size), as the original does.
// STOCK Sony libcd CdGetSector(dest, words) — the PC takes this over outright.
//
// This is the seam where a stock-libcd game becomes PC-owned. The guest routine programs DMA3 with a
// destination and a word count, spins on the CD status bit until data is ready, kicks the transfer
// and spins again until it completes. All of that is PSX hardware ceremony around ONE fact: move
// this many words of the current sector into this buffer. The port does the move and skips the
// ceremony — no controller handshake, no interrupt, no busy-wait.
//
// The LBA is NOT an argument, and that is the whole difference from the engine-loader handlers above
// (cd_read/cd_loadfile) which receive one. Stock libcd positions the head with CdlSetloc and then
// reads from wherever it was left, so the target sector is only ever stated at Setloc time —
// recorded there in Cd::setloc_lba, and consumed here. That field existed for exactly this and had
// no consumer until now.
//
// Data comes from the real disc image. A read that cannot be served fails LOUDLY and returns
// non-zero, because a zero-filled buffer reported as a successful read is indistinguishable from a
// real one to the guest and corrupts arbitrarily far downstream.
static void cd_getsector_stock(Core *c) {
  const uint32_t dest = c->r[A0], words = c->r[A1];
  Cd &cd = c->game->cd;
  if (cd.setloc_lba < 0) {
    lucent::error("cd",
                  "CdGetSector(dest=0x{:08X}, {} words) with NO Setloc — the drive was never "
                  "positioned, so there is no sector to serve. Refusing to invent one.",
                  dest,
                  words);
    c->r[V0] = 1;
    return;
  }
  uint32_t need = words * 4u;
  uint32_t done = 0;
  while (done < need) {
    // Load the next sector when the current one is exhausted. SYNC_SKIP is where the PSX data FIFO
    // begins: the 12-byte sync pattern is not presented, so the first bytes a game pops are the
    // 4-byte header (min/sec/frame/mode) followed by the 8-byte subheader.
    enum { SYNC_SKIP = 12 };
    if (cd.sec_pos >= cd.sec_len) {
      const uint32_t lba = (uint32_t)cd.setloc_lba;
      if (!disc_read_raw(&c->game->disc, lba, cd.sec_raw, sizeof cd.sec_raw)) {
        lucent::error(
            "cd", "CdGetSector: LBA {} unreadable — {} of {} bytes delivered, rest NOT written", lba, done, need);
        c->r[V0] = 1;
        return;
      }
      cd.sec_lba = (int32_t)lba;
      // The first 4 bytes a game pops are the sector HEADER (min:sec:frame in BCD, then mode), and
      // stock libcd reads exactly those to verify the drive landed where it asked. If they disagree
      // with the requested position the read is rejected and retried forever, so print them.
      lucent::debug("cd",
                    "sector LBA {} header {:02X}:{:02X}:{:02X} mode {:02X}",
                    lba,
                    cd.sec_raw[12],
                    cd.sec_raw[13],
                    cd.sec_raw[14],
                    cd.sec_raw[15]);
      cd.sec_pos = SYNC_SKIP;
      cd.sec_len = (int)sizeof cd.sec_raw;
      cd.setloc_lba = (int32_t)(lba + 1); // the head advances, as a sequential read does
    }
    uint32_t avail = (uint32_t)(cd.sec_len - cd.sec_pos);
    uint32_t n = need - done < avail ? need - done : avail;
    for (uint32_t k = 0; k < n; k++) {
      c->mem_w8(dest + done + k, cd.sec_raw[cd.sec_pos + k]);
    }
    cd.sec_pos += (int)n;
    done += n;
  }
  if (cd.verbose || lucent::channel_on("cd")) {
    lucent::info("cd",
                 "CdGetSector {} words -> 0x{:08X} (sector LBA {}, cursor {}/{})",
                 words,
                 dest,
                 cd.sec_lba,
                 cd.sec_pos,
                 cd.sec_len);
  }
  c->r[V0] = 0;
}

// STOCK Sony libcd CdRead(sectors, buf, mode) — the PC performs the ENTIRE read.
//
// Overriding here rather than at the per-sector level removes the whole guest state machine: no
// ready-callback loop, no drive-position check, no vblank-based timeout, and therefore none of the
// retry path that timeout drives. The guest asks for N sectors and gets N sectors.
//
// Bytes per sector are chosen from the MODE exactly as the guest's own code does (0x80089ECC):
//   (mode & 0x30) == 0x00 -> 0x200 words (2048) — user data only, from raw offset 24
//   (mode & 0x30) == 0x20 -> 0x249 words (2340) — whole sector, from raw offset 12 (past sync)
//   otherwise             -> 0x246 words (2328)
// Mirroring the guest's selection matters: hand it 2048-byte payloads when it asked for whole
// sectors and every header it reads back is misframed.
static void cd_read_stock(Core *c) {
  const uint32_t sectors = c->r[A0], buf = c->r[A1], mode = c->r[A2];
  Cd &cd = c->game->cd;
  if (cd.setloc_lba < 0) {
    lucent::error("cd", "CdRead({} sectors) with NO Setloc — the drive was never positioned. Refusing.", sectors);
    c->r[V0] = 0; // bool: 0 == failure, as the guest routine reports it
    return;
  }
  const uint32_t m = mode & 0x30u;
  const uint32_t bytes = (m == 0x00u) ? 2048u : (m == 0x20u) ? 2340u : 2328u;
  const uint32_t off = (m == 0x20u) ? 12u : 24u;

  uint8_t raw[2352];
  for (uint32_t i = 0; i < sectors; i++) {
    const uint32_t lba = (uint32_t)cd.setloc_lba + i;
    if (!disc_read_raw(&c->game->disc, lba, raw, sizeof raw)) {
      lucent::error("cd",
                    "CdRead: LBA {} unreadable at sector {}/{} — {} sector(s) delivered, the rest "
                    "NOT written. This read is genuinely incomplete.",
                    lba,
                    i,
                    sectors,
                    i);
      c->r[V0] = 0;
      return;
    }
    for (uint32_t k = 0; k < bytes; k++) {
      c->mem_w8(buf + i * bytes + k, raw[off + k]);
    }
  }
  cd.setloc_lba += (int32_t)sectors; // the head ends where a real sequential read would leave it
  cd.sec_pos = 0;
  cd.sec_len = 0;
  cd.sec_lba = -1;      // any per-sector FIFO state is now stale
  cd.stock_reading = 0; // this read is finished; no callback loop should run
  if (cd.verbose || lucent::channel_on("cd")) {
    lucent::info("cd",
                 "CdRead {} sector(s) x {} bytes from LBA {} -> 0x{:08X} (mode 0x{:02X})",
                 sectors,
                 bytes,
                 (int)(cd.setloc_lba - (int32_t)sectors),
                 buf,
                 mode);
  }
  c->r[V0] = 1; // bool: success
}

// CdReadSync(mode, result) -> sectors REMAINING. cd_read_stock already transferred everything
// synchronously, so the honest answer is zero. This is not a fabricated completion: the data is in
// guest memory, read from the real disc, before this ever returns.
static void cd_readsync_stock(Core *c) {
  zero_result(c, c->r[A1]);
  c->r[V0] = 0;
}

// STOCK Sony libcd CdSearchFile(CdlFILE* loc, const char* name) — resolved natively.
//
// The guest version walks the ISO filesystem by issuing real CD reads. The framework already parses
// ISO9660 directly off the disc image (disc_find_file), so the PC answers the question outright and
// no drive activity happens at all.
//
// CdlFILE is { CdlLOC pos; u_long size; char name[16] } with CdlLOC = { u_char minute, second,
// sector, track } — BCD minutes/seconds/frames. The caller's next moves confirm the layout: it
// passes this same pointer to CdControl(CdlSetloc, ...) (which consumes pos) and computes its sector
// count from the field at +4.
//
// Returns the loc pointer on success and 0 on failure, which is what the guest tests.
static void cd_searchfile_native(Core *c) {
  const uint32_t loc = c->r[A0], namep = c->r[A1];
  char name[80];
  unsigned n = 0;
  while (n + 1 < sizeof name) {
    const char ch = (char)c->mem_r8(namep + n);
    if (!ch) {
      break;
    }
    name[n++] = (ch == '\\') ? '/' : ch; // ISO paths arrive in DOS form
  }
  name[n] = 0;

  uint32_t lba = 0, size = 0;
  if (!disc_find_file(&c->game->disc, name, &lba, &size)) {
    // Not fabricating a hit: a bogus location would send the game reading arbitrary sectors.
    lucent::error("cd", "CdSearchFile: '{}' not found on the disc image", name);
    c->r[V0] = 0;
    return;
  }
  const uint32_t total = lba + 150u; // LBA -> MSF (sector 0 is 00:02:00)
  auto bcd = [](uint32_t v) {
    return (uint8_t)(((v / 10) << 4) | (v % 10));
  };
  c->mem_w8(loc + 0, bcd(total / (75u * 60u)));
  c->mem_w8(loc + 1, bcd((total / 75u) % 60u));
  c->mem_w8(loc + 2, bcd(total % 75u));
  c->mem_w8(loc + 3, 0);
  c->mem_w32(loc + 4, size);
  for (unsigned i = 0; i < 16; i++) { // name[16], NUL-padded
    c->mem_w8(loc + 8 + i, i < n ? (uint8_t)name[i] : 0);
  }
  c->game->cd.setloc_lba = (int32_t)lba; // as the guest's own version leaves it
  if (c->game->cd.verbose || lucent::channel_on("cd")) {
    lucent::info("cd", "CdSearchFile '{}' -> LBA {}, {} bytes", name, lba, size);
  }
  c->r[V0] = loc;
}

static void cd_loadfile(Core *c) {
  uint32_t dest = c->r[A0], lba = c->r[A1], size = c->r[A2];
  uint8_t sec[2048];
  uint32_t done = 0, nsec = 0;
  for (; done < size; nsec++) {
    if (!disc_read_sector(&c->game->disc, lba + nsec, sec)) {
      break;
    }
    uint32_t n = size - done < 2048 ? size - done : 2048;
    for (uint32_t j = 0; j < n; j++) {
      c->mem_w8(dest + done + j, sec[j]);
    }
    done += n;
  }
  // Position tracker: last sector read. Substrate's cd_async_read (the platform-HLE for the async
  // streaming reader B goes through) writes this; without matching writes here the SBS full-mode
  // diverges at frame 0 by two bytes at 0x800BE0E0 (native = 0, substrate = the last LBA read).
  if (nsec) {
    c->mem_w32(c->cfg->lastSectorTracker, lba + nsec - 1);
  }
  if (c->game->cd.verbose) {
    lucent::info("cd", "loadfile {} B @ LBA {} -> 0x{:08X} ra=0x{:08X}", size, lba, dest, c->r[31]);
  }
  overlay_note_load(c, dest); // record the resident overlay now (fresh image matches its signature)
  c->r[V0] = size;
}

// Direct-call native loadfile (used by the PC-native boot path, which owns the START.BIN /
// stage-overlay load top-down instead of dispatching the PSX FUN_8001db8c). Same semantics.
void Cd::loadFile(uint32_t dest, uint32_t lba, uint32_t size) {
  Core *c = &game->core;
  c->r[A0] = dest;
  c->r[A1] = lba;
  c->r[A2] = size;
  cd_loadfile(c);
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
// black. FIX (recomp-overrides, mirrors cd_loadfile): do the read natively & synchronously.
// _DAT_1f8001f4 is in 32-bit WORDS (0x200 words = 1 sector = 2048 B); copy words*4 bytes from
// consecutive sectors starting at LBA _DAT_1f8001f0 into _DAT_1f8001f8, exactly word-granular as
// FUN_8001d7c4 does (dest advances by words*4, no sector padding). Then zero the remaining count
// and advance dest/position trackers to the post-read state so FUN_8001d940's caller FUN_8001db38
// (task+0x6c is already 1 = success) sets DAT_1f80019b and ends task1.
void Cd::asyncRead() {
  Core *c = &game->core;
  uint32_t lba = c->mem_r32(0x1f8001f0);
  uint32_t words = c->mem_r32(0x1f8001f4);
  uint32_t dest = c->mem_r32(0x1f8001f8);
  uint32_t bytes = words * 4u;
  uint8_t sec[2048];
  uint32_t done = 0, nsec = 0;
  for (; done < bytes; nsec++) {
    if (!disc_read_sector(&c->game->disc, lba + nsec, sec)) {
      break;
    }
    uint32_t n = bytes - done < 2048 ? bytes - done : 2048;
    for (uint32_t j = 0; j < n; j++) {
      c->mem_w8(dest + done + j, sec[j]);
    }
    done += n;
  }
  c->mem_w32(0x1f8001f4, 0);           // remaining count consumed (callback would zero it)
  c->mem_w32(0x1f8001f8, dest + done); // dest advanced, as FUN_8001d7c4 leaves it
  if (nsec) {
    c->mem_w32(c->cfg->lastSectorTracker, lba + nsec - 1); // last sector read (pos tracker)
  }
  if (verbose) {
    lucent::info("cd", "async read {} words ({} B) @ LBA {} -> 0x{:08X}", words, bytes, lba, dest);
  }
  overlay_note_load(c, dest); // an A0* field-area code overlay may load here (MODE slot) — note it
}

// Platform-HLE entry for FUN_8001D940 (a0-less: reads the scratchpad read descriptor).
static void cd_async_read(Core *c) {
  c->game->cd.asyncRead();
}

// Direct-call native FUN_8001DC40(dest, lba, size_bytes): the inline (NON-spawning) sync reader the
// indexed file loaders use (e.g. ov_80045080). FUN_8001DC40 stuffs the scratchpad read descriptor
// (0x1f8001f8=dest, 0x1f8001f0=lba, 0x1f8001f4=ceil(size/4) words) then calls FUN_8001D940 inline; we
// reproduce that by filling the same descriptor and running the synchronous cd_async_read. Used by
// the top-down PC-driven loaders (e.g. DEMO substate s0) so they never enter the IRQ-driven reader.
void Cd::dc40Sync(uint32_t dest, uint32_t lba, uint32_t size) {
  Core *c = &game->core;
  c->mem_w32(0x1f8001f8, dest);
  c->mem_w32(0x1f8001f0, lba);
  c->mem_w32(0x1f8001f4, (size + 3u) >> 2); // ceil(size/4) words, as FUN_8001DC40 computes
  asyncRead();
  c->r[V0] = size; // FUN_8001DC40 returns size in v0
}

// Platform-HLE entry for FUN_8001DC40 (intercepted for any caller): (a0=dest, a1=lba, a2=size_bytes).
static void cd_dc40(Core *c) {
  c->game->cd.dc40Sync(c->r[A0], c->r[A1], c->r[A2]);
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
// The dialog-vs-ingame-music coordination logic (dialog tone detection, fade-in ramp, per-frame
// tick and BGM-start cut) moved to class MusicCoord in game/audio/music_coord.cpp (2026-07
// restructure) — that is game AUDIO/DIALOG DESIGN behavior, not CD-controller HLE. Reached via
// c->engine.musicCoord.{dialogToneActive/musicFadeIn/…}.

// Enable CD->SPU mixing (libsnd SpuSetCommonAttr via FUN_8001cf00(1)); needed for the SPU to
// actually mix the decoded XA (Beetle spu.c gates on SPUControl bit0). Also called from
// MusicCoord::tick() (game/audio/music_coord.cpp) on the dialog-end resume path.
void Cd::toSpuMix(int on) {
  Core *c = &game->core;
  c->r[A0] = on ? 1 : 0;
  rec_dispatch(c, 0x8001cf00u);
}

// Diagnostic: trace the game's CD-volume fade state + XA stream lifecycle, on change only.
// tgt/cur = DAT_800be222/224 (fade target/current), mas = DAT_800be220 (master),
// 19a/137 = state bytes gating FUN_80075824's ramp, song = 0x800bed80, gate = 0x801fe0e0.
// g_bgm_frame retired — c->game->timing.logicFrame.
// The gate is a lucent::Channel: this runs on the per-frame CD tick and the nine guest reads below
// are real work, so it guards the BLOCK, not a print. `PSXPORT_DEBUG=cd_override` turns it on.
void Cd::audioTrace(const char *tag) {
  Core *c = &game->core;
  static const lucent::Channel ch{"cd_override"};
  if (!ch) {
    return;
  }
  static int t = 1 << 30, cur, mas, s19a, s137, song, act, lp, gate;
  int nt = c->mem_r16s(0x800be222), ncur = c->mem_r16s(0x800be224), nmas = c->mem_r16s(0x800be220);
  int n19a = c->mem_r8(0x1f80019a), n137 = c->mem_r8(0x1f800137);
  int nsong = c->mem_r16(0x800bed80) & 0xffff, nact = xa_stream_is_active(&c->game->xa),
      nlp = xa_stream_is_looping(&c->game->xa);
  int ngate = c->mem_r16(0x801fe0e0) & 0xffff;
  if (nt != t || ncur != cur || nmas != mas || n19a != s19a || n137 != s137 || nsong != song || nact != act ||
      nlp != lp || ngate != gate) {
    lucent::debug(ch,
                  "[xa f{} {:<5}] tgt={} cur={} mas={} 19a={} 137={} song={} act={} loop={} gate={}",
                  c->game->timing.logicFrame,
                  tag ? tag : "(null)",
                  nt,
                  ncur,
                  nmas,
                  n19a,
                  n137,
                  nsong,
                  nact,
                  nlp,
                  ngate);
    t = nt;
    cur = ncur;
    mas = nmas;
    s19a = n19a;
    s137 = n137;
    song = nsong;
    act = nact;
    lp = nlp;
    gate = ngate;
  }
}

static void voice_play(Core *c) {
  uint8_t chan = (uint8_t)(c->r[A0] & 0xFF);
  uint32_t start = c->r[A1], end = c->r[A2];
  int loop = (int)(c->r[7] & 1); // a3 = flags
  lucent::debug("voice_play", "chan={} [{}..{}] loop={} ra={:08X}", chan, start, end, loop, c->r[31]);
  if (loop) { // looping clip == ingame/area background music
    c->game->cd.pending_music = 1;
    c->game->cd.pm_chan = chan;
    c->game->cd.pm_start = start;
    c->game->cd.pm_end = end;
    if (c->hooks->cdDialogToneActive(c)) {
      return; // suppress during a dialog; resumed by MusicCoord::tick
    }
  }
  xa_stream_play(&c->game->xa, chan, start, end, loop);
  c->mem_w16(0x801fe0e0, 2); // task-2 state = running (cutscene wait gate)
  c->game->cd.toSpuMix(1);
  if (loop) {
    c->hooks->cdMusicFadeIn(c); // ingame music fades in from 0 (instant-CD mod)
  }
}

// MusicCoord::cutIfDialog / MusicCoord::tick moved to game/audio/music_coord.cpp (see the header
// comment there); this file keeps only the CD-controller HLE they call into.

// 0x8001CF2C FUN_8001cf2c: stop the current voice/BGM clip.
static void voice_stop(Core *c) {
  xa_stream_stop(&c->game->xa);
  c->mem_w16(0x801fe0e0, 0);
  // EXPLICIT stop: forget any remembered looping music so the per-frame MusicCoord::tick can't
  // resurrect it. Without this, navigating the front-end menus (title<->load<->options, each exit
  // runs 0x8001cf2c) stopped the looping menu clip then immediately had it RE-PLAYED by the
  // dialog-coord resume (pending_music was still set) — the audible "menu music starts over instead
  // of stopping" bug. The in-game dialog suppression path stops via xa_stream_stop(&c->game->xa) directly (not
  // this fn) and keeps pending_music, so its resume is unaffected. Guard on !dialog: during an
  // in-game dialog the area music is suppressed+pending, and a mid-dialog 0x8001cf2c (line change)
  // must NOT forget it, or it wouldn't resume when the dialog ends.
  if (!c->hooks->cdDialogToneActive(c)) {
    c->game->cd.pending_music = 0;
  }
  c->r[A0] = 0;
  rec_dispatch(c, 0x8001cf00u); // CD->SPU mix off
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
void Cd::hleInit() {
  Core *c = &game->core;
  // FUN_800898a0 success path (0x800898c4..0x800898fc): install the CD-event callback table.
  const GameConfig *cfg = c->cfg;
  c->mem_w32(cfg->cdCallbackTable[0], cfg->cdCallbackFn[0]); // CD-ready / sync callback
  c->mem_w32(cfg->cdCallbackTable[1], cfg->cdCallbackFn[1]); // CD-ready-cb 2
  c->mem_w32(cfg->cdCallbackTable[2], cfg->cdCallbackFn[2]); // CD event handler
  c->mem_w32(cfg->cdCallbackTable[3], cfg->cdCallbackFn[3]); // (cleared)
  if (verbose || lucent::channel_on("cd")) {
    lucent::info("cd", "HLE CdInit: drive ready (no controller, no handshake, no busy-wait)");
  }
}

// Deliver more streamed sectors by invoking the ready callback the guest registered.
//
// A stream is not a file read. A file read is finite and terminates itself, which is why
// cd_drive_stock_read can run it to completion in one burst. A stream runs until the guest says
// stop, and it expects its callback invoked repeatedly at roughly the drive's sector rate — so it
// must be pumped from the port's timing, not from the command that started it.
//
// The guest's own Pause/Stop clears stream_active, so this never outlives what the game asked for.
// ---- streamed-read drive pacing (declared in cd.h; gated by tests/test_cd_stream_drive_rate.cpp) --
int cd_stream_sectors_per_sec(uint8_t mode) {
  return (mode & 0x80) ? 150 : 75; // 75 sectors/s per speed multiple; bit 0x80 = double speed
}

int cd_stream_sectors_due(uint64_t elapsed_ns, int sectors_per_sec, uint32_t already_delivered) {
  if (sectors_per_sec <= 0) {
    return 0;
  }
  const uint64_t owed = elapsed_ns * (uint64_t)sectors_per_sec / 1000000000ull;
  if (owed <= (uint64_t)already_delivered) {
    return 0; // caught up or ahead — never negative
  }
  uint64_t due = owed - (uint64_t)already_delivered;
  if (due > (uint64_t)CD_STREAM_MAX_BURST) {
    due = CD_STREAM_MAX_BURST;
  }
  return (int)due;
}

void Cd::pumpStream(Core *c, int sectors) {
  const GameConfig *cfg = c->cfg;
  if (!stream_active || !cfg || !cfg->cdReadyCbPtr) {
    return;
  }
  const uint32_t cb = c->mem_r32(cfg->cdReadyCbPtr);
  if (!cb) {
    return;
  }

  // PACE IT AT THE DRIVE'S RATE. The caller asks for `sectors`; the DRIVE decides how many it can
  // actually have delivered by now. Without this the stream ran as fast as the guest asked and the
  // movies played ~4x too fast against their own (correctly paced) audio — see cd_stream_sectors_due.
  //
  // A refusal here is not a failure: StGetNext's honest answer becomes "no sector ready", which is
  // exactly what the guest sees on hardware between sectors, and it spins in its own loop until the
  // drive catches up. The host still gets its turn at every recompiled function entry, so this
  // cannot wedge.
  {
    const uint64_t now_ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
    if (stream_t0_ns == 0) {
      stream_t0_ns = now_ns; // first pump of this stream seeds the clock
    }
    const uint8_t mode = game ? game->cdc.mode : 0x80;
    const int rate = cd_stream_sectors_per_sec(mode);
    int due = cd_stream_sectors_due(now_ns - stream_t0_ns, rate, stream_delivered);
    // The very first sector is never withheld: at t=0 nothing has elapsed, so nothing is owed, and
    // a stream that cannot deliver its first sector can never start.
    if (stream_delivered == 0 && due == 0) {
      due = 1;
    }
    if (due <= 0) {
      lucent::debug("cdpace",
                    "holding: {} sector(s) delivered in {} ms at {}/s — the drive is ahead",
                    stream_delivered,
                    (now_ns - stream_t0_ns) / 1000000ull,
                    rate);
      return;
    }
    if (sectors > due) {
      sectors = due;
    }
  }

  // The callback runs as an ordinary guest function; save and restore the whole register context
  // around it so whatever the port interrupted sees nothing.
  const R3000 saved = *static_cast<R3000 *>(c);
  for (int i = 0; i < sectors && stream_active; i++) {
    stream_delivered++;
    c->r[A0] = 1; // libcd passes the completion status as arg 1
    c->r[A1] = 0;
    rec_dispatch(c, cb);
  }
  *static_cast<R3000 *>(c) = saved;
}

void Cd::overridesInit() {
  if (lucent::channel_on("cd")) {
    verbose = 1;
  }
  // All CD-subsystem HLE handlers register with this Game's PlatformHle table (class in
  // platform_hle.h). Every entry is an I/O primitive in the platform-HLE window (0x8001Cxxx
  // engine CD glue / 0x8008xxxx libcd) — the FAIL-FAST sync model: every CD op is served
  // natively + synchronously, so the libcd IRQ/VSync busy-waits (CdSync/CdCommand) are never reached.
  //   0x8008B2D8 (CdInit handshake) is owned by PlatformHle::initBuiltins (cdinit_hs) — don't
  //   double-register here.
  PlatformHle &hle = game->platform_hle;
  const GameConfig *cfg = game->core.cfg;
  // Skip an address the game has not configured. Zero means "this game has no such CD primitive, or
  // it has not been RE'd yet" — the same convention GameConfig::hle uses. Passing 0 straight through
  // made register_() emit "REFUSED 0x00000000" once per unconfigured entry, which is pure noise that
  // looks like a real error: a port with no CD group RE'd yet printed nine of them at every boot and
  // buried whatever the next diagnostic was.
  auto reg = [&](uint32_t addr, OverrideFn fn) {
    if (addr) {
      hle.register_(addr, fn);
    }
  };
  reg(cfg->cdInlineLoad, cd_dc40);              // inline async loader -> sync
  reg(cfg->voicePlay, voice_play);              // voice/BGM clip player -> native xa_stream
  reg(cfg->voiceStop, voice_stop);              // stop voice/BGM -> native
  reg(cfg->cdFileLoad, cd_loadfile);            // engine file loader -> sync sector read
  reg(cfg->cdCommand, cd_command);              // libcd CdCommand -> success (no controller)
  reg(cfg->cdSync, cd_sync);                    // libcd CdSync -> complete (CD is synchronous)
  reg(cfg->cdCmdStream, cd_cmd_stream);         // streaming CD-cmd wrapper (GetlocL pos in range)
  reg(cfg->cdReadPrim, cd_read);                // libcd by-LBA read -> native sync
  reg(cfg->cdGetSector, cd_getsector_stock);    // STOCK libcd CdGetSector(dest, words) -> native
  reg(cfg->cdReadStock, cd_read_stock);         // STOCK libcd CdRead(sectors, buf, mode) -> native
  reg(cfg->cdReadSync, cd_readsync_stock);      // STOCK libcd CdReadSync -> complete
  reg(cfg->cdSearchFile, cd_searchfile_native); // STOCK libcd CdSearchFile -> native ISO9660 lookup
  reg(cfg->cdAsyncRead, cd_async_read);         // async streaming reader -> sync (area-DATA load)
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

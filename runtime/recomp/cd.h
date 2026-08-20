// cd.h — class Cd — the native CD subsystem, owned by Game (`c->game->cd`, back-pointer wired in
// Game()). Implemented in cd_override.cpp: the synchronous native read primitives the PC-native
// loaders call directly, the CD-subsystem PlatformHle registrations (libcd command/sync/read
// leaves), and the deferred ingame-music state (a looping clip suppressed during a dialog is
// remembered here and resumed by MusicCoord::tick).
#pragma once
#include <cstdint>
class Game;
class Core;

// ---- streamed-read DRIVE PACING ----------------------------------------------------------------
// A continuous read (XA / STR) is paced by the DRIVE, not by how fast the guest asks for sectors.
// Cd::pumpStream used to deliver one every time the guest's StGetNext found none ready, with no rate
// limit at all, so a movie ran as fast as the host could walk the file: measured on spider1, the
// guest's data head covered 2040 sectors of CINEMAS/ATVILOGO.STR while the real-time-paced XA audio
// head covered 512 — video 3.98x too fast, reproduced at 3.97x on LOGO.STR. Audio can never sync to
// that, and nothing was wrong with the audio.
//
// These two are pure functions of hardware facts and elapsed time, so they are gated by a hermetic
// test (tests/test_cd_stream_drive_rate.cpp) rather than reasoned about.

// Sectors per second for a Setmode byte. A CD-ROM delivers 75 sectors/s per speed multiple, and
// Setmode bit 0x80 selects double speed. Not a tunable: the guest programs mode 0xE0 for these
// movies, which is double speed.
int cd_stream_sectors_per_sec(uint8_t mode);

// How many sectors the drive owes RIGHT NOW: elapsed x rate, minus what has already been delivered
// for this stream. Never negative (a caller uses it directly as a loop count) and bounded per call,
// so a long stall cannot be repaid as one unpaced burst — which would be the very behaviour this
// exists to stop.
int cd_stream_sectors_due(uint64_t elapsed_ns, int sectors_per_sec, uint32_t already_delivered);

// Per-call bound on the catch-up above. One video field of double-speed delivery (150/60 = 2.5,
// rounded up): enough that a stream pumped once per field keeps up exactly, and small enough that a
// backlog is repaid over several fields instead of in one burst.
#define CD_STREAM_MAX_BURST 3

class Cd {
public:
  Game *game = nullptr;
  // deferred ingame-music state (suppressed during dialog, resumed after)
  int pending_music = 0; // a looping ingame-music clip is deferred/remembered (was s_pending_music)
  uint8_t pm_chan = 0;   // was s_pm_chan
  uint32_t pm_start = 0; // was s_pm_start
  uint32_t pm_end = 0;   // was s_pm_end
  int verbose = 0;       // [cd] read/loadfile trace (was s_cd_verbose)

  // Drive position last set by CdlSetloc (command 0x02), as an LBA; -1 = none set yet.
  //
  // Why this exists: a game built on an ENGINE loader hands the read primitive its LBA directly
  // (dest, lba, size), so the framework's cd_read/cd_loadfile handlers get it as an argument. A game
  // built on STOCK Sony libcd does not — there the sequence is CdControl(CdlSetloc, msf) to position
  // the drive, then a read that transfers from wherever the head was left. The LBA is simply not an
  // argument to the read, so without remembering it here a stock-libcd read has no idea what to fetch.
  //
  // cd_command already intercepts Setloc, but only forwarded it to the XA audio streamer; that path
  // is for cutscene BGM/voice and keeps no DATA read position. Recording it costs nothing, is pure
  // bookkeeping (no guest memory is touched), and is what a stock-libcd read path needs to exist at
  // all. Surfaced by the Spyro port, which spins forever after its boot splash because no read can
  // resolve a target sector.
  int32_t setloc_lba = -1;

  // Stock-libcd read driving (cd_override.cpp cd_drive_stock_read). `stock_reading` is cleared by the
  // guest's own Pause/Stop, which is what terminates the per-sector callback loop; `in_stock_read`
  // stops a callback that re-issues ReadN from starting a nested loop.
  int stock_reading = 0;
  int in_stock_read = 0;

  // A CONTINUOUS read (XA / STR streaming) is active. Distinct from a file read, which is finite and
  // self-terminating: the guest asks for N sectors and stops. A stream expects its ready callback to
  // keep being invoked for as long as it runs, paced by the drive — so it needs a periodic pump
  // rather than the burst a file read gets. Set on ReadN/ReadS, cleared by the guest's own Pause/Stop.
  int stream_active = 0;

  // Drive-rate pacing for that stream (see cd_stream_sectors_due above). `stream_t0_ns` is the
  // steady-clock reading when the stream started and `stream_delivered` counts the sectors handed to
  // the guest since then; together they say whether the drive is ahead of or behind real time.
  // Reset on every ReadN/ReadS so a new movie starts with a fresh budget rather than inheriting the
  // last one's credit.
  uint64_t stream_t0_ns = 0;
  uint32_t stream_delivered = 0;

  // Sector FIFO for the stock-libcd path. Real hardware presents ONE sector as a byte stream that
  // successive CdGetSector calls pop SEQUENTIALLY — the game reads 3 words (the 4-byte header plus
  // 8-byte subheader, to verify the drive position) and then 512 words (the user data) out of the
  // same sector. Serving every call from offset 0 hands the position check user data, it disagrees
  // with the expected sector number, and the read is rejected and retried forever.
  //
  // So the buffer is RAW (2352 bytes) and a cursor tracks how much has been popped, starting past
  // the 12-byte sync pattern — which is exactly where the PSX data FIFO begins in whole-sector mode.
  uint8_t sec_raw[2352] = {};
  int sec_pos = 0;      // cursor into sec_raw; 0 = nothing loaded
  int sec_len = 0;      // bytes available in sec_raw
  int32_t sec_lba = -1; // which LBA sec_raw holds

  // loadFile(dest, lba, size): direct-call native loadfile (0x8001DB8C semantics) — used by the
  //   PC-native boot/stage path, which owns the START.BIN / stage-overlay load top-down.
  void loadFile(uint32_t dest, uint32_t lba, uint32_t size);
  // asyncRead(): 0x8001D940 FUN_8001d940, the engine's async streaming reader, done natively &
  //   synchronously from the scratchpad read descriptor (0x1f8001f0/f4/f8).
  void asyncRead();
  // dc40Sync(dest, lba, size): direct-call native FUN_8001DC40 — fill the scratchpad read
  //   descriptor and run the synchronous asyncRead; returns size in guest V0.
  void dc40Sync(uint32_t dest, uint32_t lba, uint32_t size);
  // toSpuMix(on): enable/disable CD->SPU mixing (libsnd SpuSetCommonAttr via FUN_8001cf00(1));
  //   needed for the SPU to actually mix the decoded XA (Beetle spu.c gates on SPUControl bit0).
  //   Also called from MusicCoord::tick() on the dialog-end resume path.
  void toSpuMix(int on);
  // audioTrace(tag): diagnostic — trace the game's CD-volume fade state + XA stream lifecycle,
  //   on change only (`PSXPORT_DEBUG=cd_override`).
  void audioTrace(const char *tag);
  // hleInit(): native HLE CdInit — leave RAM in the state FUN_800898a0's SUCCESS path leaves it
  //   (CD-event callback table installed); no controller handshake, no busy-wait.
  void hleInit();
  // overridesInit(): register every CD-subsystem PlatformHle handler with this Game's table.
  void overridesInit();
  // pumpStream(c, sectors): deliver up to `sectors` more streamed sectors to the guest by invoking
  //   the ready callback it registered. No-op unless a continuous read is active. Call from the
  //   port's per-field timing so the stream advances at roughly the drive's rate; a file read must
  //   NOT be pumped this way (it terminates itself).
  void pumpStream(Core *c, int sectors);
};

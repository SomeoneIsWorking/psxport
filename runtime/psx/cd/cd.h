// cd.h — class Cd — the stock libcd state of one Game (`c->game->cd`, back-pointer wired in Game()).
// The libcd entries are psx::cd::StockLibcd; a title's own CD loaders live in the title.
#pragma once

#include <array>
#include <cstdint>
class Game;
class Core;
namespace psx::cd {
class StockLibcd;
}

// ---- streamed-read DRIVE PACING ----------------------------------------------------------------
// A continuous read (XA / STR) is paced by the DRIVE, not by how fast the guest asks for sectors.
// The stream pump used to deliver one every time the guest's StGetNext found none ready, with no rate
// limit at all, so a movie ran as fast as the host could walk the file: measured on spider1, the
// guest's data head covered 2040 sectors of CINEMAS/ATVILOGO.STR while the real-time-paced XA audio
// head covered 512 — video 3.98x too fast, reproduced at 3.97x on LOGO.STR. Audio can never sync to
// that, and nothing was wrong with the audio.
//
// The hardware rate is owned by cd_drive_timing; the stream budget below applies that shared rate to
// elapsed time. Both are gated by tests/test_cd_stream_drive_rate.cpp rather than reasoned about.

// How many sectors the drive owes RIGHT NOW: elapsed x rate, minus what has already been delivered
// for this stream. Never negative (a caller uses it directly as a loop count) and bounded per call,
// so a long stall cannot be repaid as one unpaced burst — which would be the very behaviour this
// exists to stop.
namespace psx::cd {
// Elapsed time since the stream started and the sectors already delivered for it.
struct StreamProgress {
  uint64_t elapsedNs = 0;
  uint32_t delivered = 0;
};
int streamSectorsDue(StreamProgress progress, int sectorsPerSecond);
} // namespace psx::cd

// Per-call bound on the catch-up above. One video field of double-speed delivery (150/60 = 2.5,
// rounded up): enough that a stream pumped once per field keeps up exactly, and small enough that a
// backlog is repaid over several fields instead of in one burst.
#define CD_STREAM_MAX_BURST 3

class Cd {
public:
  Game *game = nullptr;

  // Last synchronous stock-libcd command response. CdSync can receive a distinct guest
  // buffer after CdCommand and must publish the same completed result there.
  std::array<uint8_t, 8> stock_command_response{};
  bool stock_command_response_valid = false;

  // Drive position last set by CdlSetloc (command 0x02), as an LBA; -1 = none set yet. Stock libcd
  // reads from wherever the head was left, so the read has no LBA argument of its own.
  int32_t setloc_lba = -1;

  // Stock-libcd read driving (StockLibcd::driveFiniteRead). `stock_reading` is cleared by the
  // guest's own Pause/Stop, which is what terminates the per-sector callback loop; `in_stock_read`
  // stops a callback that re-issues ReadN from starting a nested loop.
  int stock_reading = 0;
  int in_stock_read = 0;

  // A CONTINUOUS read (XA / STR streaming) is active. Distinct from a file read, which is finite and
  // self-terminating: the guest asks for N sectors and stops. A stream expects its ready callback to
  // keep being invoked for as long as it runs, paced by the drive — so it needs a periodic pump
  // rather than the burst a file read gets. Set on ReadN/ReadS, cleared by the guest's own Pause/Stop.
  int stream_active = 0;

  // Drive-rate pacing for that stream (see psx::cd::streamSectorsDue above). `stream_t0_ns` is the
  // steady-clock reading when the stream started and `stream_delivered` counts host-dispatched ready
  // callback attempts since then; together they cap the direct-callback path's host rate. Guest
  // interrupt delivery uses the controller's own deadline and does not increment this count.
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

  // The title's stock libcd (psx::cd::StockLibcd::bind); null when it bound none.
  void bindStockLibrary(const psx::cd::StockLibcd &library) {
    stockLibrary_ = &library;
  }
  const psx::cd::StockLibcd *stockLibrary() const {
    return stockLibrary_;
  }

private:
  const psx::cd::StockLibcd *stockLibrary_ = nullptr;
};

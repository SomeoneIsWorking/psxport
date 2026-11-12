// class Cd: the stock libcd state of one Game. The libcd entries are psx::cd::StockLibcd; a title's own CD
// loaders live in the title.
#pragma once

#include "cd_position.h"
#include "field_rate.h"

#include <array>
#include <cstdint>
#include <optional>
class Game;
class Core;
namespace psx::cd {
class StockLibcd;

// A continuous read's progress since it started: guest display fields elapsed and sectors the head passed.
struct StreamProgress {
  uint64_t elapsedFields = 0;
  uint32_t passed = 0;
};
// Where a host-pumped stream's pacing starts: the field and head LBA of its first pump.
struct StreamOrigin {
  uint64_t field = 0;
  uint32_t lba = 0;
};
// Sectors the drive owes now at `sectorsPerSecond`, at most kStreamMaxBurst so a stall is repaid over fields.
int streamSectorsDue(StreamProgress progress, int sectorsPerSecond, psx::frame::DisplayFieldRate fieldRate);
// One field of double-speed delivery (150/60), rounded up.
inline constexpr int kStreamMaxBurst = 3;
} // namespace psx::cd

class Cd {
public:
  // The last synchronous stock command response, which a later CdSync publishes into its own buffer.
  class StockResponse {
  public:
    using Bytes = std::array<uint8_t, 8>;

    void set(const Bytes &bytes) {
      bytes_ = bytes;
      valid_ = true;
    }
    void clear() {
      bytes_.fill(0);
      valid_ = false;
    }
    // Writes the retained response, zeros when none, to the guest buffer at `result` (0 = none).
    void publish(Core &core, uint32_t result) const;
    const Bytes &bytes() const {
      return bytes_;
    }
    bool valid() const {
      return valid_;
    }

  private:
    Bytes bytes_{};
    bool valid_ = false;
  };

  Game *game = nullptr;
  StockResponse stockResponse;

  // CdlSetloc's LBA; -1 = none. Stock libcd reads from wherever the head was left.
  int32_t setloc_lba = -1;

  // StockLibcd::driveFiniteRead: the guest's Pause/Stop clears stock_reading; in_stock_read stops a nested loop.
  int stock_reading = 0;
  int in_stock_read = 0;

  // A ReadN/ReadS stream that wants its ready callback until the guest's Pause/Stop.
  int stream_active = 0;
  // Guest-field pacing of a host-pumped stream; reset by every ReadN/ReadS.
  std::optional<psx::cd::StreamOrigin> stream_origin;

  // CdGetSector pops one raw sector sequentially from past the sync, as the whole-sector data FIFO does.
  uint8_t sec_raw[psx::cd::kRawSectorBytes] = {};
  int sec_pos = 0;
  int sec_len = 0;
  int32_t sec_lba = -1;

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

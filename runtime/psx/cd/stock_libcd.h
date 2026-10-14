// stock_libcd.h — Sony libcd's stock entries served by the framework's synchronous CD model.
#pragma once

#include "cd_ready_delivery.h"
#include "execution_exit.h"
#include "stock_cd_work_area.h"

#include <cstdint>

class Core;
class Game;

namespace psx::cd {

// The title owns one (in its runtime or session) with the libcd addresses measured in its executable and
// binds it from registerOverrides; a zero entry is not linked and stays guest code. This base calls the ready
// callback from the host at the drive's rate; GuestInterruptLibcd leaves it to the guest's INT1 service.
class StockLibcd {
public:
  struct Entries {
    std::uint32_t read = 0;            // CdRead(sectors, buf, mode)
    std::uint32_t readSync = 0;        // CdReadSync(mode, result)
    std::uint32_t command = 0;         // CD_cw(cmd, param, result)
    std::uint32_t control = 0;         // CdControl(cmd, param, result)
    std::uint32_t controlBlocking = 0; // CdControlB(cmd, param, result)
    std::uint32_t controlFire = 0;     // CdControlF(cmd, param)
    std::uint32_t sync = 0;            // CdSync(noblock, result)
    std::uint32_t searchFile = 0;      // CdSearchFile(loc, name)
    std::uint32_t getSector = 0;       // CdGetSector(dest, words)
  };

  // `readySlot` is libcd's CdReadyCallback pointer; zero means the title has none.
  StockLibcd(const Entries &entries, const StockCommandWorkArea &workArea, std::uint32_t readySlot)
      : entries_(entries), workArea_(workArea), readySlot_(readySlot) {}
  virtual ~StockLibcd() = default;
  StockLibcd(const StockLibcd &) = delete;
  StockLibcd &operator=(const StockLibcd &) = delete;
  StockLibcd(StockLibcd &&) = delete;
  StockLibcd &operator=(StockLibcd &&) = delete;

  // Registers the entries on `game` and makes this its libcd; must outlive the Game's guest execution.
  void bind(Game &game) const &;
  // The Game keeps a pointer to it, so a temporary cannot bind.
  void bind(Game &game) const && = delete;

  // The libcd bodies, also for a title's own wrappers: guest arguments in a0-a2, result in v0.
  void read(Core &core) const;
  void readSync(Core &core) const;
  void command(Core &core) const;     // v0 = 0
  void control(Core &core) const;     // v0 = 1
  void controlFire(Core &core) const; // no result buffer
  void sync(Core &core) const;        // v0 = 2
  void searchFile(Core &core) const;
  void getSector(Core &core) const;

  std::uint32_t readySlot() const {
    return readySlot_;
  }

  // Services a continuous read that owes up to `sectors` ready callbacks.
  virtual void pumpStream(Core &core, int sectors) const;
  // Whether the guest's CD interrupt delivers the ready callback (then deliverOnInterrupt does).
  virtual bool deliversOnInterrupt() const {
    return false;
  }
  // One owed data-ready completion, after the guest's own interrupt elements declined the CD bit.
  virtual CdReadyDelivery deliverOnInterrupt(Core &core) const {
    (void)core;
    return CdReadyDelivery::NotOwned;
  }

protected:
  // One host-pumped ready call for `callback`; libcd's data-ready code and no sector pointer.
  virtual psx::cpu::ExecutionResult callReady(Core &core, std::uint32_t callback) const;
  // A ReadN/ReadS began. Unless CdRead is native, a finite read runs its ready callback to the guest's Pause.
  virtual void readStarted(Core &core) const;
  // A whole CdRead of `sectors` from `firstLba` landed in guest RAM.
  virtual void readLanded(Core &core, std::uint32_t firstLba, std::uint32_t sectors, std::uint8_t mode) const {
    (void)core;
    (void)firstLba;
    (void)sectors;
    (void)mode;
  }

private:
  void applyCommand(Core &core) const;
  void driveFiniteRead(Core &core) const;

  Entries entries_;
  StockCommandWorkArea workArea_;
  std::uint32_t readySlot_;
};

// A libcd whose INT1 service consumes the controller response and then calls the ready callback; the
// framework stands in for the BIOS CD-ROM handler that runs it (cd_ready_delivery.h).
class GuestInterruptLibcd : public StockLibcd {
public:
  struct Completion {
    // `$a0` the INT1 service hands the callback, read from the callback's own bytes.
    std::uint8_t status = 1;
    // A synchronous CdRead still owes one INT1 completion, for a guest that chains reads from it.
    bool perStockRead = false;
  };

  GuestInterruptLibcd(const Entries &entries,
                      const StockCommandWorkArea &workArea,
                      std::uint32_t readySlot,
                      const Completion &completion)
      : StockLibcd(entries, workArea, readySlot), completion_(completion) {}

  void pumpStream(Core &core, int sectors) const override;
  bool deliversOnInterrupt() const override {
    return readySlot() != 0;
  }
  CdReadyDelivery deliverOnInterrupt(Core &core) const override;

protected:
  void readStarted(Core &core) const override;
  void readLanded(Core &core, std::uint32_t firstLba, std::uint32_t sectors, std::uint8_t mode) const override;

private:
  Completion completion_;
};

} // namespace psx::cd

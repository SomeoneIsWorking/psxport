#include "stock_libcd.h"

#include "cd_drive_timing.h"
#include "cd_position.h"
#include "cd_stock_read_completion.h"
#include "core.h"
#include "execution_control.h"
#include "game.h"
#include "guest_call.h"
#include "platform_hle.h"
#include "stock_cd_response.h"
#include "xa_state.h"

#include <algorithm>
#include <chrono>
#include <lucent/log.h>

namespace psx::cd {
namespace {

enum Register : int { V0 = 2, A0 = 4, A1 = 5, A2 = 6 };

// Upper bound on one finite read's ready-callback burst: larger than any single PSX read.
constexpr unsigned kMaxBurstSectors = 65536;

const StockLibcd &libcdOf(const Core *core) {
  return *core->game->cd.stockLibrary();
}

void readEntry(Core *core) {
  libcdOf(core).read(*core);
}
void readSyncEntry(Core *core) {
  libcdOf(core).readSync(*core);
}
void commandEntry(Core *core) {
  libcdOf(core).command(*core);
}
void controlEntry(Core *core) {
  libcdOf(core).control(*core);
}
void controlFireEntry(Core *core) {
  libcdOf(core).controlFire(*core);
}
void syncEntry(Core *core) {
  libcdOf(core).sync(*core);
}
void searchFileEntry(Core *core) {
  libcdOf(core).searchFile(*core);
}
void getSectorEntry(Core *core) {
  libcdOf(core).getSector(*core);
}

// Applies a command's position parameter to the XA cursor and the data read position. One owner for
// the explicit Setloc and the implicit one a position-carrying command performs (issue 0115).
void positionFromParam(Core &core, std::uint32_t param, const char *why) {
  if (param == 0) {
    return;
  }
  const auto mm = static_cast<std::uint8_t>(core.mem_r8(param));
  const auto ss = static_cast<std::uint8_t>(core.mem_r8(param + 1));
  const auto ff = static_cast<std::uint8_t>(core.mem_r8(param + 2));
  Cd &cd = core.game->cd;
  xa_stream_setloc(&core.game->xa, mm, ss, ff);
  // Repositioning invalidates the buffered sector.
  cd.sec_pos = 0;
  cd.sec_len = 0;
  cd.sec_lba = -1;
  cd.setloc_lba = msfToLba(mm, ss, ff);
  lucent::debug("cd", "{} {:02X}:{:02X}:{:02X} -> LBA {}", why, mm, ss, ff, cd.setloc_lba);
}

std::uint64_t steadyNowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

} // namespace

void StockLibcd::bind(Game &game) const & {
  game.cd.bindStockLibrary(*this);
  PlatformHle &table = game.platform_hle;
  table.bindStock(entries_.read, readEntry);
  table.bindStock(entries_.readSync, readSyncEntry);
  table.bindStock(entries_.command, commandEntry);
  table.bindStock(entries_.control, controlEntry);
  table.bindStock(entries_.controlBlocking, controlEntry);
  table.bindStock(entries_.controlFire, controlFireEntry);
  table.bindStock(entries_.sync, syncEntry);
  table.bindStock(entries_.searchFile, searchFileEntry);
  table.bindStock(entries_.getSector, getSectorEntry);
}

psx::cpu::ExecutionResult StockLibcd::callReady(Core &core, std::uint32_t callback) const {
  constexpr std::uint32_t kDataReady = 1u;
  return psx::cpu::dispatchGuest2(core, callback, kDataReady, 0u, psx::cpu::ExecutionBudget::currentTurn(core));
}

void StockLibcd::readStarted(Core &core) const {
  if (entries_.read == 0) {
    driveFiniteRead(core);
  }
}

// A finite read without a native CdRead: call the guest's ready callback once per sector until its own
// Pause/Stop clears `stock_reading`. The register context is restored around the burst.
void StockLibcd::driveFiniteRead(Core &core) const {
  if (!readySlot_) {
    return;
  }
  Cd &cd = core.game->cd;
  if (cd.in_stock_read) {
    return; // the callback re-issued a read; the outer loop runs it
  }
  cd.in_stock_read = 1;
  cd.stock_reading = 1;
  const R3000 saved = *static_cast<R3000 *>(&core);
  unsigned n = 0;
  for (; n < kMaxBurstSectors && cd.stock_reading; n++) {
    const std::uint32_t callback = core.mem_r32(readySlot_);
    if (!callback) {
      break;
    }
    const auto result = callReady(core, callback);
    if (!result.returned()) {
      *static_cast<R3000 *>(&core) = saved;
      cd.in_stock_read = 0;
      psx::cpu::completeOrPropagate(core, result);
      return;
    }
  }
  *static_cast<R3000 *>(&core) = saved;
  cd.in_stock_read = 0;
  if (n >= kMaxBurstSectors) {
    lucent::error("cd",
                  "stock read did not terminate after {} sectors; the guest never issued Pause/Stop. Read abandoned; "
                  "treat its data as incomplete.",
                  n);
  } else {
    lucent::debug("cd", "stock read complete: {} ready-callback invocation(s)", n);
  }
}

// Everything a controller command does to native state, shared by the entries with and without a result.
void StockLibcd::applyCommand(Core &core) const {
  const std::uint32_t cmd = core.r[A0] & 0xFF;
  const std::uint32_t param = core.r[A1];
  if (lucent::channel_on("cdcmd")) {
    std::uint8_t p[4] = {0, 0, 0, 0};
    if (param) {
      for (std::uint32_t i = 0; i < 4; i++) {
        p[i] = static_cast<std::uint8_t>(core.mem_r8(param + i));
      }
    }
    lucent::debug("cdcmd",
                  "cmd=0x{:02X} param=[{:02X} {:02X} {:02X} {:02X}] mode={} ra=0x{:08X}",
                  cmd,
                  p[0],
                  p[1],
                  p[2],
                  p[3],
                  core.r[7],
                  core.r[31]);
  }
  Game &game = *core.game;
  Cd &cd = game.cd;
  const std::uint8_t p0 = param ? static_cast<std::uint8_t>(core.mem_r8(param)) : 0;
  // The replaced command-send recorded Setloc/Setmode in guest RAM; libcd reads them back later.
  publishStockCommandWorkArea(core, workArea_, static_cast<std::uint8_t>(cmd), param);
  switch (cmd) {
  case 0x0E: // Setmode, to the XA streamer and the controller (0x20 selects whole-sector framing)
    xa_stream_setmode(&game.xa, p0);
    cdc_set_mode(&game.cdc, p0);
    break;
  case 0x0D: // Setfilter
    xa_stream_setfilter(&game.xa, p0, param ? static_cast<std::uint8_t>(core.mem_r8(param + 1)) : 0);
    cdc_set_filter(&game.cdc, p0, param ? static_cast<std::uint8_t>(core.mem_r8(param + 1)) : 0);
    break;
  case 0x02:
    positionFromParam(core, param, "setloc");
    break;
  case 0x03: // SetlocL
  case 0x15: // SeekL
  case 0x16: // SeekP
    positionFromParam(core, param, "implicit setloc");
    break;
  case 0x06:
  case 0x1B: // ReadN / ReadS: libcd's CdControl sends the implicit Setloc first (issue 0115)
    positionFromParam(core, param, "implicit setloc");
    xa_stream_start(&game.xa);
    // The controller reads too: streaming code waits on its DRQSTS bit before kicking DMA3.
    if (cd.setloc_lba >= 0) {
      cdc_begin_read(&game.cdc, static_cast<std::uint32_t>(cd.setloc_lba));
    }
    cd.stream_active = 1;
    // A new stream starts with a fresh pacing budget.
    cd.stream_t0_ns = 0;
    cd.stream_delivered = 0;
    readStarted(core);
    break;
  case 0x08:
  case 0x09: // Stop / Pause: the guest's end of a read and of a stream
    xa_stream_stop(&game.xa);
    cd.stock_reading = 0;
    cd.stream_active = 0;
    cdc_issue_command(&game.cdc, static_cast<std::uint8_t>(cmd));
    break;
  default:
    break;
  }
}

void StockLibcd::command(Core &core) const {
  applyCommand(core);
  core.r[V0] = stock_cd_begin_command(core, static_cast<std::uint8_t>(core.r[A0]), core.r[A1], core.r[A2]) ? 0u : 1u;
}

void StockLibcd::control(Core &core) const {
  applyCommand(core);
  core.r[V0] = stock_cd_begin_command(core, static_cast<std::uint8_t>(core.r[A0]), core.r[A1], core.r[A2]) ? 1u : 0u;
}

// CdControlF(com, param) has no result: a2 is whatever the caller left there.
void StockLibcd::controlFire(Core &core) const {
  applyCommand(core);
  core.r[V0] = stock_cd_begin_command(core, static_cast<std::uint8_t>(core.r[A0]), core.r[A1], 0u) ? 1u : 0u;
}

void StockLibcd::sync(Core &core) const {
  stock_cd_publish_sync(core, core.r[A1]);
  core.r[V0] = 2;
}

// CdGetSector(dest, words): pops the sector at the Setloc position as the data FIFO presents it, the
// header and subheader first (past the 12-byte sync), advancing the head per whole sector.
void StockLibcd::getSector(Core &core) const {
  const std::uint32_t dest = core.r[A0];
  const std::uint32_t words = core.r[A1];
  Cd &cd = core.game->cd;
  if (cd.setloc_lba < 0) {
    lucent::error("cd", "CdGetSector(dest=0x{:08X}, {} words) with no Setloc; no sector to serve", dest, words);
    core.r[V0] = 1;
    return;
  }
  const std::uint32_t need = words * 4u;
  std::uint32_t done = 0;
  while (done < need) {
    constexpr int kSyncSkip = 12;
    if (cd.sec_pos >= cd.sec_len) {
      const auto lba = static_cast<std::uint32_t>(cd.setloc_lba);
      if (!disc_read_raw(&core.game->disc, lba, cd.sec_raw, sizeof cd.sec_raw)) {
        lucent::error(
            "cd", "CdGetSector: LBA {} unreadable; {} of {} bytes delivered, rest not written", lba, done, need);
        core.r[V0] = 1;
        return;
      }
      cd.sec_lba = static_cast<std::int32_t>(lba);
      lucent::debug("cd",
                    "sector LBA {} header {:02X}:{:02X}:{:02X} mode {:02X}",
                    lba,
                    cd.sec_raw[12],
                    cd.sec_raw[13],
                    cd.sec_raw[14],
                    cd.sec_raw[15]);
      cd.sec_pos = kSyncSkip;
      cd.sec_len = static_cast<int>(sizeof cd.sec_raw);
      cd.setloc_lba = static_cast<std::int32_t>(lba + 1);
    }
    const auto avail = static_cast<std::uint32_t>(cd.sec_len - cd.sec_pos);
    const std::uint32_t n = need - done < avail ? need - done : avail;
    core.mem_write(dest + done,
                   std::span<const std::uint8_t>(cd.sec_raw + cd.sec_pos, n),
                   psx::cpu::ExecutableWriteSource::ModuleLoad);
    cd.sec_pos += static_cast<int>(n);
    done += n;
  }
  lucent::debug("cd",
                "CdGetSector {} words -> 0x{:08X} (sector LBA {}, cursor {}/{})",
                words,
                dest,
                cd.sec_lba,
                cd.sec_pos,
                cd.sec_len);
  core.r[V0] = 0;
}

// CdRead(sectors, buf, mode): the whole read, synchronously, from the Setloc position. Bytes per sector
// follow the guest's own mode selection: 2048 from offset 24, 2340 from 12 (whole sector), else 2328.
void StockLibcd::read(Core &core) const {
  const std::uint32_t sectors = core.r[A0];
  const std::uint32_t buf = core.r[A1];
  const std::uint32_t mode = core.r[A2];
  Cd &cd = core.game->cd;
  if (cd.setloc_lba < 0) {
    lucent::error("cd", "CdRead({} sectors) with no Setloc; the drive was never positioned", sectors);
    core.r[V0] = 0;
    return;
  }
  const std::uint32_t m = mode & 0x30u;
  const std::uint32_t bytes = (m == 0x00u) ? 2048u : (m == 0x20u) ? 2340u : 2328u;
  const std::uint32_t off = (m == 0x20u) ? 12u : 24u;

  std::uint8_t raw[2352];
  for (std::uint32_t i = 0; i < sectors; i++) {
    const std::uint32_t lba = static_cast<std::uint32_t>(cd.setloc_lba) + i;
    // Through the controller's sector source, so a hermetic test can feed fake sectors.
    if (!core.game->cdc.disc_read_raw_fn(&core.game->disc, lba, raw, sizeof raw)) {
      lucent::error("cd",
                    "CdRead: LBA {} unreadable at sector {}/{}; {} sector(s) delivered, the rest not written",
                    lba,
                    i,
                    sectors,
                    i);
      core.r[V0] = 0;
      return;
    }
    // A guest may stream a code module through CdRead (Tomba! 1 does), so each sector is reported.
    core.mem_write(
        buf + i * bytes, std::span<const std::uint8_t>(raw + off, bytes), psx::cpu::ExecutableWriteSource::ModuleLoad);
  }
  cd.setloc_lba += static_cast<std::int32_t>(sectors);
  cd.sec_pos = 0;
  cd.sec_len = 0;
  cd.sec_lba = -1;
  cd.stock_reading = 0;
  const std::uint32_t firstLba = static_cast<std::uint32_t>(cd.setloc_lba) - sectors;
  lucent::debug("cd",
                "CdRead {} sector(s) x {} bytes from LBA {} -> 0x{:08X} (mode 0x{:02X})",
                sectors,
                bytes,
                firstLba,
                buf,
                mode);
  announceStockReadLanding(core,
                           {.firstLba = firstLba, .sectors = sectors, .destination = buf, .bytes = sectors * bytes});
  readLanded(core, firstLba, sectors, static_cast<std::uint8_t>(mode));
  core.r[V0] = 1;
}

// CdReadSync(mode, result): CdRead already transferred everything, so nothing remains.
void StockLibcd::readSync(Core &core) const {
  stock_cd_zero_result(core, core.r[A1]);
  core.r[V0] = 0;
}

// CdSearchFile(CdlFILE *loc, name) from the disc's ISO9660 index: fills {CdlLOC pos; size; name[16]},
// leaves the head at the file and returns `loc`, or 0 when the file is absent.
void StockLibcd::searchFile(Core &core) const {
  const std::uint32_t loc = core.r[A0];
  const std::uint32_t namep = core.r[A1];
  char name[80];
  unsigned n = 0;
  while (n + 1 < sizeof name) {
    const auto ch = static_cast<char>(core.mem_r8(namep + n));
    if (!ch) {
      break;
    }
    name[n++] = (ch == '\\') ? '/' : ch;
  }
  name[n] = 0;

  std::uint32_t lba = 0;
  std::uint32_t size = 0;
  if (!disc_find_file(&core.game->disc, name, &lba, &size)) {
    lucent::error("cd", "CdSearchFile: '{}' not found on the disc image", name);
    core.r[V0] = 0;
    return;
  }
  const std::uint32_t total = lba + 150u; // sector 0 is 00:02:00
  auto bcd = [](std::uint32_t v) {
    return static_cast<std::uint8_t>(((v / 10) << 4) | (v % 10));
  };
  core.mem_w8(loc + 0, bcd(total / (75u * 60u)));
  core.mem_w8(loc + 1, bcd((total / 75u) % 60u));
  core.mem_w8(loc + 2, bcd(total % 75u));
  core.mem_w8(loc + 3, 0);
  core.mem_w32(loc + 4, size);
  for (unsigned i = 0; i < 16; i++) {
    core.mem_w8(loc + 8 + i, i < n ? static_cast<std::uint8_t>(name[i]) : 0);
  }
  core.game->cd.setloc_lba = static_cast<std::int32_t>(lba);
  lucent::debug("cd", "CdSearchFile '{}' -> LBA {}, {} bytes", name, lba, size);
  core.r[V0] = loc;
}

// A continuous read (XA/STR) runs until the guest's Pause/Stop and is paced by the drive: at most the
// sectors the drive owes by now, and each one only once the controller holds its data-ready response.
void StockLibcd::pumpStream(Core &core, int sectors) const {
  Cd &cd = core.game->cd;
  if (!cd.stream_active || sectors <= 0 || !readySlot_) {
    return;
  }
  const std::uint32_t callback = core.mem_r32(readySlot_);
  if (!callback) {
    return;
  }
  const std::uint64_t nowNs = steadyNowNs();
  if (cd.stream_t0_ns == 0) {
    cd.stream_t0_ns = nowNs;
  }
  Game &game = *core.game;
  const int rate = cd_drive_sectors_per_second(game.cdc.mode);
  int due = streamSectorsDue(nowNs - cd.stream_t0_ns, rate, cd.stream_delivered);
  // At t = 0 nothing is owed yet, and a stream that cannot deliver its first sector never starts.
  if (cd.stream_delivered == 0 && due == 0) {
    due = 1;
  }
  if (due <= 0) {
    lucent::debug("cdpace",
                  "holding: {} sector(s) delivered in {} ms at {}/s; the drive is ahead",
                  cd.stream_delivered,
                  (nowNs - cd.stream_t0_ns) / 1000000ull,
                  rate);
    return;
  }
  sectors = std::min(sectors, due);
  const R3000 saved = *static_cast<R3000 *>(&core);
  for (int i = 0; i < sectors && cd.stream_active; i++) {
    game.timing.serviceCdcTickSource();
    if (cdc_current_irq_type(&game.cdc) != 1u) {
      break;
    }
    cd.stream_delivered++;
    const auto result = callReady(core, callback);
    if (!result.returned()) {
      *static_cast<R3000 *>(&core) = saved;
      psx::cpu::completeOrPropagate(core, result);
      return;
    }
  }
  *static_cast<R3000 *>(&core) = saved;
}

// The controller raises INT1 on its own drive clock and the guest's service delivers it, so a stream
// only advances that clock.
void GuestInterruptLibcd::pumpStream(Core &core, int sectors) const {
  if (core.game->cd.stream_active && sectors > 0) {
    core.game->timing.serviceCdcTickSource();
  }
}

// The controller's own data-ready response delivers a read; a host burst would deliver it twice.
void GuestInterruptLibcd::readStarted(Core &core) const {
  (void)core;
}

void GuestInterruptLibcd::readLanded(Core &core,
                                     std::uint32_t firstLba,
                                     std::uint32_t sectors,
                                     std::uint8_t mode) const {
  if (completion_.perStockRead && deliversOnInterrupt()) {
    raiseStockReadCompletion(core, firstLba, sectors, mode);
  }
}

CdReadyDelivery GuestInterruptLibcd::deliverOnInterrupt(Core &core) const {
  if (!deliversOnInterrupt()) {
    return CdReadyDelivery::NotOwned;
  }
  return deliverReadyCompletion(core, readySlot(), completion_.status);
}

int streamSectorsDue(uint64_t elapsedNs, int sectorsPerSecond, uint32_t alreadyDelivered) {
  if (sectorsPerSecond <= 0) {
    return 0;
  }
  const uint64_t owed = elapsedNs * static_cast<uint64_t>(sectorsPerSecond) / 1000000000ull;
  if (owed <= static_cast<uint64_t>(alreadyDelivered)) {
    return 0;
  }
  const uint64_t due = owed - static_cast<uint64_t>(alreadyDelivered);
  return static_cast<int>(due > static_cast<uint64_t>(CD_STREAM_MAX_BURST) ? CD_STREAM_MAX_BURST : due);
}

} // namespace psx::cd

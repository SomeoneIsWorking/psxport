// psx::cd::CdController: the CXD1199-style CDROM register model at 0x1F801800-3, timed on the guest clock.
//
//   0x1F801800  W: index/bank (low 2 bits)      R: status (FIFO/busy bits + index)
//   0x1F801801  W bank0: command                R: response FIFO (pop)
//   0x1F801802  W bank0: parameter FIFO (push)   R: data FIFO (pop)
//   0x1F801803  W bank0: request (BFRD want-data) W bank1: ack/reset IRQ flags
//               R bank0/2: interrupt enable      R bank1/3: interrupt flag (pending IRQ type)
#include "cd_drive_timing.h"
#include "cd_position.h"
#include "cdc_command_phase.h"
#include "cdc_state.h"
#include "disc.h"
#include "disc_toc.h"
#include "emulated_time.h"
#include "xa_adpcm.h"
#include "xa_state.h"
#include <lucent/log.h>
#include <optional>
#include <string.h>

namespace psx::cd {
namespace {

constexpr uint8_t kCdlStatStandby = 0x02;
constexpr uint8_t kCdlStatRead = 0x20;
// Interrupt-enable bits 0-4: the five response types (INT1..INT5 are encoded in bits 0-2).
constexpr uint8_t kCdcIrqEnableAll = 0x1F;
// INT5 error code for an argument the command cannot satisfy (GetTD of a track the disc lacks).
constexpr uint8_t kCdcErrorBadArgument = 0x10;

} // namespace

void CdController::reset() {
  Disc *const wiredDisc = disc;
  psx::audio::XaStream *const wiredXa = xa;
  *this = CdController{};
  stat = kCdlStatStandby;
  // As the replaced BIOS CD initialisation leaves it.
  irq_en = kCdcIrqEnableAll;
  disc = wiredDisc;
  xa = wiredXa;
  disc_read_raw_fn = [](Disc *d, uint32_t lba, uint8_t *out, uint32_t n) {
    return d->readRaw(lba, out, n);
  };
  disc_get_subq_position_fn = [](Disc *d, uint32_t lba, uint8_t *out) {
    return d->subqPosition(lba, out);
  };
}

bool CdController::queueEmpty() const {
  return q_head == q_tail;
}

// The interrupt line: the current response's type ANDed with the enable register (Beetle RecalcIRQ). A masked
// response stays readable through the flag register; Spyro 3's disc check polls GetTN that way.
bool CdController::irqLine() const {
  return !queueEmpty() && (q[q_head].type & irq_en & kCdcIrqEnableAll) != 0;
}

// A masked response raises no I_STAT edge.
void CdController::responseBecameCurrent() {
  ++irq_sequence;
  if (irqLine()) {
    irq_edge = 1;
  }
}

void CdController::raise(CdcResponse type, const uint8_t *resp, int len) {
  const bool becomes_current = q_tail == q_head;
  int n = (q_tail + 1) & 7;
  if (n == q_head) {
    return;
  }
  q[q_tail].type = static_cast<uint8_t>(type);
  q[q_tail].len = len;
  if (len) {
    memcpy(q[q_tail].resp, resp, (size_t)len);
  }
  if (becomes_current) {
    resp_rd = 0;
  }
  q_tail = n;
  if (becomes_current) {
    responseBecameCurrent();
  }
}

// An acknowledged response stays readable until the next one becomes current (Beetle BeginResults).
const CdcIrqEnt *CdController::resultsEntry() const {
  return &q[queueEmpty() ? ((q_head - 1) & 7) : q_head];
}

CdcResponse CdController::currentIrqType() const {
  return queueEmpty() ? CdcResponse::None : static_cast<CdcResponse>(q[q_head].type);
}

uint64_t CdController::seekTime(uint32_t target_lba) const {
  // Beetle's PS_CDC_CalcSeekTime with only its random 0..25000 component fixed at zero.
  const bool motor_on = (stat & kCdlStatStandby) != 0;
  const bool paused = motor_on && !reading;
  // A Setloc into the lead-in is a negative LBA carried in two's complement.
  const int64_t initial_lba = motor_on ? static_cast<int32_t>(loc_lba) : 0;
  const int64_t signed_target_lba = static_cast<int32_t>(target_lba);
  const uint64_t difference = static_cast<uint64_t>(initial_lba > signed_target_lba ? initial_lba - signed_target_lba
                                                                                    : signed_target_lba - initial_lba);
  constexpr uint64_t kCpuHz = psx::frame::kNominalPsxCpuHz;
  uint64_t ticks = motor_on ? 0u : kCpuHz;
  const uint64_t seek_ticks = difference * kCpuHz / (uint64_t{72} * kSecondsPerMinute * kSectorsPerSecond);
  ticks += seek_ticks < 20'000u ? 20'000u : seek_ticks;
  if (difference >= 2'250u) {
    ticks += kCpuHz * 300u / 1'000u;
  } else if (paused) {
    ticks += (mode & kModeDoubleSpeed) != 0 ? 1'237'952u : 1'237'952u * 2u;
  } else if (difference >= 3u && difference < 12u) {
    ticks += driveSectorPeriodCpuTicks(mode) * 4u;
  }
  return ticks;
}

namespace {

struct ArgumentCounts {
  uint8_t min = 0;
  uint8_t max = 0;
};

// The parameter counts a command accepts; none for an unknown command.
std::optional<ArgumentCounts> commandArgumentCounts(uint8_t command) {
  switch (command) {
  case 0x02:
    return ArgumentCounts{3, 3};
  case 0x03:
    return ArgumentCounts{0, 1};
  case 0x0D:
  case 0x1D:
    return ArgumentCounts{2, 2};
  case 0x0E:
  case 0x12:
  case 0x14:
  case 0x19:
    return ArgumentCounts{1, 1};
  case 0x01:
  case 0x04:
  case 0x05:
  case 0x06:
  case 0x07:
  case 0x08:
  case 0x09:
  case 0x0A:
  case 0x0B:
  case 0x0C:
  case 0x0F:
  case 0x10:
  case 0x11:
  case 0x13:
  case 0x15:
  case 0x16:
  case 0x1A:
  case 0x1B:
  case 0x1C:
  case 0x1E:
    return ArgumentCounts{0, 0};
  default:
    return std::nullopt;
  }
}

} // namespace

// STRSND routes a Mode2 sector whose submode has all of RT|form2|audio (0x64); the filter drops other channels.
CdcSectorRoute CdController::sectorRoute(const uint8_t *raw) const {
  const SectorSubheader header = sectorSubheader(raw);
  if ((mode & kModeXaAdpcm) == 0 || !isMode2(raw) || (header.submode & 0x64) != 0x64) {
    return CdcSectorRoute::Data;
  }
  const bool selected = (mode & kModeXaFilter) == 0 || (header.file == filter_file && header.channel == filter_chan);
  return selected ? CdcSectorRoute::XaAudio : CdcSectorRoute::XaDropped;
}

// An audio sector is always consumed: an overflowing SPU CD buffer drops samples while the drive keeps reading.
DriveSector CdController::routeDriveSector(uint32_t lba, uint8_t (&raw)[kRawSectorBytes]) {
  if (!disc_read_raw_fn(disc, lba, raw, kRawSectorBytes)) {
    return DriveSector::ReadFailed;
  }
  memcpy(sector_header, raw + kSectorHeaderOffset, kSectorHeaderBytes);
  sector_header_valid = 1;
  const CdcSectorRoute route = sectorRoute(raw);
  if (route == CdcSectorRoute::Data) {
    return DriveSector::Data;
  }
  if (route == CdcSectorRoute::XaAudio) {
    xa->pushAudioSector(raw, lba);
  }
  return DriveSector::Audio;
}

// The sector at `lba` during a continuous read: audio never reaches the guest's FIFO or raises INT1.
DriveSector CdController::consumeSector(uint32_t lba) {
  uint8_t raw[kRawSectorBytes];
  const DriveSector sector = routeDriveSector(lba, raw);
  if (sector == DriveSector::ReadFailed) {
    lucent::error("cdc", "read: no data for LBA {}; data FIFO left empty", lba);
    data_n = 0;
    data_rd = 0;
  } else if (sector == DriveSector::Data) {
    fillDataFifo(raw);
  }
  return sector;
}

// Streaming code tells video from audio by the subheader whole-sector framing puts first.
void CdController::fillDataFifo(const uint8_t (&raw)[kRawSectorBytes]) {
  const SectorFraming framing = sectorFraming(mode);
  const SectorSubheader header = sectorSubheader(raw);
  lucent::debug("cdc",
                "sector file={} chan={} submode=0x{:02X} -> data FIFO {}@{}",
                header.file,
                header.channel,
                header.submode,
                framing.bytes,
                framing.offset);
  memcpy(data, raw + framing.offset, framing.bytes);
  data_n = static_cast<int>(framing.bytes);
  data_rd = 0;
}

bool CdController::loadSector(uint32_t lba) {
  uint8_t raw[kRawSectorBytes];
  if (!disc_read_raw_fn(disc, lba, raw, kRawSectorBytes)) {
    lucent::error("cdc", "read: no data for LBA {}; data FIFO left empty", lba);
    data_n = 0;
    data_rd = 0;
    return false;
  }
  fillDataFifo(raw);
  return true;
}

void CdController::queueDataReady() {
  const uint8_t response[1] = {stat};
  raise(CdcResponse::DataReady, response, 1);
}

bool CdController::postDataReady() {
  if (((q_tail + 1) & 7) == q_head) {
    return 0;
  }
  queueDataReady();
  return 1;
}

bool CdController::postDataReadyAfterRead(uint32_t first_lba, uint32_t sectors, uint8_t mode) {
  if (sectors == 0 || read_completion_n >= 4) {
    return 0;
  }
  if (!tick_now) {
    return postDataReady();
  }
  const uint64_t now_ticks = tick_now(tick_context);
  const uint64_t start = read_completion_n ? read_completion_deadline_ticks[read_completion_n - 1] : now_ticks;
  read_completion_deadline_ticks[read_completion_n++] = (start > now_ticks ? start : now_ticks) + seekTime(first_lba) +
                                                        uint64_t{sectors} * driveSectorPeriodCpuTicks(mode);
  loc_lba = first_lba + sectors;
  stat |= kCdlStatStandby;
  return 1;
}

// Announce every owed read completion whose drive time has elapsed, oldest first. A full response queue
// leaves the rest owed for the next service rather than dropping one.
bool CdController::serviceReadCompletions() {
  if (read_completion_n == 0 || !tick_now) {
    return 0;
  }
  const uint64_t now_ticks = tick_now(tick_context);
  int announced = 0;
  while (read_completion_n > 0 && now_ticks >= read_completion_deadline_ticks[0] && postDataReady()) {
    lucent::debug("cdcpace", "announced owed read completion now={} queue_empty={}", now_ticks, queueEmpty());
    read_completion_n--;
    for (int i = 0; i < read_completion_n; i++) {
      read_completion_deadline_ticks[i] = read_completion_deadline_ticks[i + 1];
    }
    announced = 1;
  }
  return announced;
}

void CdController::cancelDriveEvent() {
  if (!drive_event_armed) {
    return;
  }
  drive_event_armed = 0;
  drive_deadline_ticks = 0;
}

// The next sector is due one drive period after `origin`.
void CdController::scheduleSectorEventAfter(uint64_t origin) {
  if (!reading || following_sector_ready || drive_event_armed || !tick_now) {
    return;
  }
  drive_deadline_ticks = origin + driveSectorPeriodCpuTicks(mode);
  drive_event_armed = 1;
  lucent::debug("cdcpace",
                "armed sector event origin={} deadline={} mode=0x{:02X} first={}",
                origin,
                drive_deadline_ticks,
                mode,
                first_sector_pending);
}

void CdController::scheduleSectorEvent() {
  if (!tick_now) {
    return;
  }
  scheduleSectorEventAfter(tick_now(tick_context));
}

// A guest Pause/Stop is answered by its own command responses. A drive that stopped on an unreadable sector
// raises INT1 with the read bit cleared (Beetle cdc.c CDCIRQ_DATA_END), which libcd's CdlReadS waits for.
// True when that INT1 was queued, so the service latches the line now.
bool CdController::stopContinuousRead(ReadStop stop) {
  cancelDriveEvent();
  reading = 0;
  stat &= static_cast<uint8_t>(~kCdlStatRead);
  first_sector_pending = 0;
  if (stop == ReadStop::GuestCommand) {
    // An announced sector stays in the drive buffer for the guest's BFRD.
    return 0;
  }
  following_sector_ready = 0;
  if (postDataReady() == 0) {
    lucent::error("cdc",
                  "the drive stopped at LBA {} with the response queue full; the guest will not learn the read ended",
                  loc_lba);
    return 0;
  }
  return 1;
}

void CdController::startContinuousRead() {
  cancelDriveEvent();
  sector_header_valid = 0; // the seek leaves the latched sector
  reading = 1;
  stat |= kCdlStatRead;
  first_sector_pending = 1;
  bfrd = 0;
  following_sector_ready = 0;
  data_n = 0;
  data_rd = 0;
  scheduleSectorEvent();
}

void CdController::bindTickSource(void *context, CdcTickNowFn now) {
  tick_context = context;
  tick_now = now;
}

// One sector whose deadline has passed; an audio sector re-arms from its own deadline so a late service
// still passes every sector the head reached.
bool CdController::serviceDueSector(uint64_t now_ticks) {
  const uint64_t due_ticks = drive_deadline_ticks;
  drive_event_armed = 0;
  drive_deadline_ticks = 0;
  lucent::debug("cdcpace", "servicing sector event now={} first={} lba={}", now_ticks, first_sector_pending, loc_lba);
  if (!reading || following_sector_ready) {
    return 0;
  }

  if (first_sector_pending) {
    first_sector_pending = 0;
    const DriveSector sector = consumeSector(loc_lba);
    if (sector == DriveSector::ReadFailed) {
      return stopContinuousRead(ReadStop::DriveStopped);
    }
    // Before queueDataReady, which publishes stat as the INT1 status byte.
    stat |= kCdlStatRead;
    if (sector == DriveSector::Audio) {
      scheduleSectorEventAfter(due_ticks);
      lucent::debug("cdcpace", "first sector @ {} was XA audio -> SPU ring", loc_lba);
      return 0;
    }
    queueDataReady();
    return 1;
  }

  // One drive buffer, one INT1: the guest's next BFRD request swaps the ready sector into the FIFO. Only
  // STRSND can make the next sector audio, which the drive consumes without an interrupt.
  if (mode & kModeXaAdpcm) {
    uint8_t raw[kRawSectorBytes];
    const DriveSector sector = routeDriveSector(loc_lba + 1, raw);
    if (sector == DriveSector::ReadFailed) {
      return stopContinuousRead(ReadStop::DriveStopped);
    }
    if (sector == DriveSector::Audio) {
      loc_lba++;
      scheduleSectorEventAfter(due_ticks);
      return 0;
    }
  }
  following_sector_ready = 1;
  queueDataReady();
  return 1;
}

bool CdController::serviceDriveEvent() {
  if (!drive_event_armed || !tick_now) {
    return 0;
  }
  const uint64_t now_ticks = tick_now(tick_context);
  while (drive_event_armed && now_ticks >= drive_deadline_ticks) {
    if (serviceDueSector(now_ticks) != 0) {
      return 1;
    }
  }
  return 0;
}

// Draining the FIFO releases the buffer; the next BFRD request installs an announced sector.
void CdController::sectorConsumed() {
  data_n = 0;
  data_rd = 0;
  bfrd = 0;
}

void CdController::setMode(uint8_t value) {
  lucent::debug("cdc",
                "setmode 0x{:02X} (speed={} xa-adpcm={} filter={} whole-sector={})",
                value,
                (value & kModeDoubleSpeed) ? 2 : 1,
                (value & kModeXaAdpcm) ? 1 : 0,
                (value & kModeXaFilter) ? 1 : 0,
                (value & kModeWholeSector) ? 1 : 0);
  mode = value;
  if ((value & kModeXaAdpcm) == 0 && xa != nullptr) {
    xa->stopForMode(value);
  }
  if (drive_event_armed) {
    cancelDriveEvent();
    scheduleSectorEvent();
  }
}

void CdController::setFilter(uint8_t file, uint8_t channel) {
  filter_file = file;
  filter_chan = channel;
  lucent::debug("cdc", "setfilter file={} chan={}", file, channel);
}

// Runs a still-pending command through its own execute/complete phases now, so a read applied
// synchronously after it cannot be cancelled by it (C-12's Pause before ReadN).
void CdController::retirePendingCommand() {
  if (!command_event_armed || !tick_now) {
    return;
  }
  for (int guard = 0; guard < 64 && command_event_armed; ++guard) {
    command_deadline_ticks = tick_now(tick_context);
    const CdcCommandEvent event = serviceCommand(1);
    if (event == CdcCommandEvent::Execute) {
      finishCommandExecution(execCommand(pending_command));
    } else if (event == CdcCommandEvent::Complete) {
      completeCommand();
    }
  }
}

void CdController::beginRead(uint32_t lba) {
  retirePendingCommand();
  loc_lba = lba;
  command_lba = lba;
  startContinuousRead();
}

void CdController::issueCommand(uint8_t command) {
  index = 0;
  // Timed like a register write; the guest that issued it consumes both responses.
  scheduleCommand(command);
}

int CdController::takeCurrentResponse(uint8_t *bytes, int capacity) {
  const int savedBank = index;
  int taken = 0;
  while ((read(CdcRegister::Index) & kCdcStatResponseReady) != 0u) {
    const uint8_t value = (uint8_t)read(CdcRegister::Response);
    if (bytes && taken < capacity) {
      bytes[taken] = value;
    }
    taken++;
  }
  write(CdcRegister::Index, (uint8_t)savedBank);
  return taken;
}

// DMA3 drains the FIFO, then reads zeros (Beetle PS_CDC_DMARead); returns the FIFO-sourced words.
int CdController::dmaRead(uint32_t *out, int words) {
  if (words <= 0) {
    return 0;
  }
  memset(out, 0, (size_t)words * sizeof *out);
  if (!bfrd) {
    return 0;
  }
  int got = 0;
  while (got < words && data_rd + 4 <= data_n) {
    const uint8_t *p = data + data_rd;
    out[got++] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    data_rd += 4;
  }
  if (got && data_rd >= data_n) {
    sectorConsumed();
  }
  return got;
}

uint64_t CdController::execCommand(uint8_t cmd) {
  lucent::debug("cdc",
                "cmd 0x{:02X} args={} [{:02X} {:02X} {:02X}]",
                cmd,
                command_arg_n,
                command_args[0],
                command_args[1],
                command_args[2]);
  const std::optional<ArgumentCounts> counts = commandArgumentCounts(cmd);
  if (!counts || command_arg_n < counts->min || command_arg_n > counts->max) {
    const uint8_t error[2] = {static_cast<uint8_t>(stat | 0x01), 0x20};
    raise(CdcResponse::Error, error, 2);
    return 0;
  }
  uint8_t r1[1] = {stat};
  switch (cmd) {
  case 0x01:
    raise(CdcResponse::Acknowledge, r1, 1);
    return 0; // Getstat
  case 0x02:
    command_lba = static_cast<uint32_t>(msfToLba(command_args[0], command_args[1], command_args[2]));
    raise(CdcResponse::Acknowledge, r1, 1);
    return 0; // Setloc
  case 0x0E:
    setMode(command_args[0]);
    raise(CdcResponse::Acknowledge, r1, 1);
    return 0; // Setmode
  case 0x0D:
    setFilter(command_args[0], command_args[1]);
    raise(CdcResponse::Acknowledge, r1, 1);
    return 0;  // Setfilter
  case 0x10: { // GetlocL
    uint8_t header[kSectorHeaderBytes] = {};
    if (!getlocL(header)) {
      const uint8_t error[2] = {static_cast<uint8_t>(stat | 0x01), 0x80};
      raise(CdcResponse::Error, error, 2);
      return 0;
    }
    raise(CdcResponse::Acknowledge, header, kSectorHeaderBytes);
    return 0;
  }
  case 0x11: { // GetlocP: current Sub-Q track/index and relative/absolute position.
    uint8_t position[8] = {};
    if (!getlocP(position)) {
      const uint8_t error[2] = {static_cast<uint8_t>(stat | 0x01), 0x80};
      raise(CdcResponse::Error, error, 2);
      return 0;
    }
    raise(CdcResponse::Acknowledge, position, 8);
    return 0;
  }
  case 0x06:
  case 0x1B: // ReadN / ReadS
    raise(CdcResponse::Acknowledge, r1, 1);
    loc_lba = command_lba;
    startContinuousRead();
    return 0;
  case 0x09: {
    const uint8_t reading_status[1] = {stat};
    const bool was_reading = reading != 0;
    stopContinuousRead(ReadStop::GuestCommand);
    raise(CdcResponse::Acknowledge, reading_status, 1);
    if (!was_reading) {
      return 5'000;
    }
    const uint64_t speed_scale = (mode & kModeDoubleSpeed) != 0 ? 1u : 2u;
    return (1'124'584u + static_cast<uint64_t>(loc_lba) * 42'596u / (uint64_t{kSectorsPerSecond} * kSecondsPerMinute)) *
           speed_scale;
  }
  case 0x08: {
    const uint8_t reading_status[1] = {stat};
    const bool was_stopped = !reading && (stat & kCdlStatStandby) == 0;
    stopContinuousRead(ReadStop::GuestCommand);
    stat = 0;
    sector_header_valid = 0;
    raise(CdcResponse::Acknowledge, reading_status, 1);
    return was_stopped ? 5'000u : 33'868u;
  }
  case 0x0A:
    stopContinuousRead(ReadStop::GuestCommand);
    stat = kCdlStatStandby;
    mode = 0;
    command_lba = 0;
    raise(CdcResponse::Acknowledge, r1, 1);
    return 4'100'000u; // Reset's deterministic seek-to-sector-zero completion.
  case 0x0B:
  case 0x0C:
    raise(CdcResponse::Acknowledge, r1, 1);
    return 0; // Mute / Demute
  // SeekL keeps the latched header (Beetle cdc.c Command_SeekL).
  case 0x15:
    raise(CdcResponse::Acknowledge, r1, 1);
    return seekTime(command_lba);
  case 0x16: // SeekP
    sector_header_valid = 0;
    raise(CdcResponse::Acknowledge, r1, 1);
    return seekTime(command_lba);
  case 0x1E: // ReadTOC
    sector_header_valid = 0;
    raise(CdcResponse::Acknowledge, r1, 1);
    return 30'000'000u + seekTime(0);
  case 0x07:
    if (stat != 0) {
      const uint8_t error[2] = {static_cast<uint8_t>(stat | 0x01), 0x20};
      raise(CdcResponse::Error, error, 2);
      return 0;
    }
    stat = kCdlStatStandby;
    raise(CdcResponse::Acknowledge, r1, 1);
    return 3'386'880u;
  case 0x03: // Play seeks to the audio track
    sector_header_valid = 0;
    raise(CdcResponse::Acknowledge, r1, 1);
    return 0;
  case 0x17:
  case 0x18: // no retail command
    raise(CdcResponse::Acknowledge, r1, 1);
    return 0;
  case 0x19: { // Test (sub-function in param[0])
    if (command_args[0] == 0x20) {
      uint8_t v[4] = {0x94, 0x09, 0x19, 0xC0}; // BIOS date/version
      raise(CdcResponse::Acknowledge, v, 4);
    } else {
      raise(CdcResponse::Acknowledge, r1, 1);
    }
    return 0;
  }
  case 0x1A: { // GetID -> region/license
    raise(CdcResponse::Acknowledge, r1, 1);
    return 33'868u;
  }
  case 0x12:
    raise(CdcResponse::Acknowledge, r1, 1);
    return 33'868u;
  case 0x13:   // GetTN
  case 0x14: { // GetTD
    psx::disc_toc::BcdPair answer{};
    const bool known = disc != nullptr && (cmd == 0x13 ? psx::disc_toc::track_range(*disc, answer)
                                                       : psx::disc_toc::track_start(*disc, command_args[0], answer));
    if (!known) {
      const uint8_t error[2] = {static_cast<uint8_t>(stat | 0x01), kCdcErrorBadArgument};
      raise(CdcResponse::Error, error, 2);
      return 0;
    }
    const uint8_t t[3] = {stat, answer.first, answer.second};
    raise(CdcResponse::Acknowledge, t, 3);
    return 0;
  }
  default:
    lucent::debug("cdc", "UNHANDLED cmd 0x{:02X} -> ack only", cmd);
    raise(CdcResponse::Acknowledge, r1, 1);
    return 0;
  }
}

void CdController::completeCommand() {
  if (pending_command == 0x1A) {
    const uint8_t id[8] = {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'};
    raise(CdcResponse::Complete, id, 8);
    return;
  }
  if (pending_command == 0x0A) {
    loc_lba = 0;
  } else if (pending_command == 0x15 || pending_command == 0x16) {
    loc_lba = command_lba;
    stat = kCdlStatStandby;
  }
  const uint8_t response[1] = {stat};
  raise(CdcResponse::Complete, response, 1);
}

bool CdController::getlocL(uint8_t out[8]) const {
  if (sector_header_valid == 0) {
    return false;
  }
  memcpy(out, sector_header, kSectorHeaderBytes);
  return true;
}

bool CdController::getlocP(uint8_t out[8]) const {
  return disc_get_subq_position_fn != nullptr && disc_get_subq_position_fn(disc, loc_lba, out);
}

bool CdController::nextDeadlineTicks(uint64_t *absoluteTicks) const {
  // A Lightrec segment ends at the earliest armed deadline.
  uint64_t earliest = 0;
  bool armed = false;
  const auto consider = [&](bool isArmed, uint64_t deadline) {
    if (isArmed && (!armed || deadline < earliest)) {
      earliest = deadline;
      armed = true;
    }
  };
  consider(drive_event_armed != 0, drive_deadline_ticks);
  consider(command_event_armed != 0, command_deadline_ticks);
  consider(read_completion_n != 0, read_completion_deadline_ticks[0]);
  if (!armed) {
    return 0;
  }
  *absoluteTicks = earliest;
  return 1;
}

bool CdController::service() {
  // Drive before command, as the oracle: a tie makes INT1 current and queues INT3 behind it.
  const uint64_t irq_sequence_before = irq_sequence;
  serviceDriveEvent();
  serviceReadCompletions();
  const CdcCommandEvent event = serviceCommand(queueEmpty());
  if (event == CdcCommandEvent::Execute) {
    finishCommandExecution(execCommand(pending_command));
  } else if (event == CdcCommandEvent::Complete) {
    completeCommand();
  }
  return irq_sequence != irq_sequence_before;
}

// The bytes a consumer reads of a presented sector; a request past them asks for the next sector.
std::uint32_t CdController::sectorPayloadBytes() const {
  return (mode & kModeWholeSector) != 0 ? 12u + kDataSectorBytes : kDataSectorBytes;
}

// BFRD is a level: a repeated 0x80 keeps the FIFO and cursor, but still presents an announced sector.

void CdController::writeRequestRegister(uint8_t value) {
  const uint8_t asserted = value & 0x80;
  if (!asserted) {
    bfrd = 0;
    // A clear without bit 6 empties the FIFO but keeps the sector for a re-request (Beetle).
    if ((value & 0x40) == 0) {
      data_rd = data_n;
    }
    return;
  }
  const bool flushed = data_n > 0 && data_rd >= data_n;

  // Stock libcd writes the bit once per sector and never clears it (psxport issue 0002).
  const bool fresh_request = bfrd == 0;
  bfrd = 1;
  if (!reading && !following_sector_ready) {
    if (flushed) {
      data_rd = 0;
    }
    return;
  }

  // The announced sector replaces one whose payload the guest has read (C-12 reads past it into the EDC tail).
  if (following_sector_ready && (data_n == 0 || data_rd >= sectorPayloadBytes())) {
    loc_lba++;
    following_sector_ready = 0;
    if (!loadSector(loc_lba)) {
      stopContinuousRead(ReadStop::DriveStopped);
      return;
    }
    // With one drive buffer, taking the announced sector is what lets the drive read the next one.
    scheduleSectorEvent();
  } else {
    if (flushed) {
      data_rd = 0;
    }
    if (fresh_request && data_n > 0 && !following_sector_ready) {
      scheduleSectorEvent();
    }
  }
}

uint32_t CdController::read(CdcRegister reg) {
  switch (reg) {
  case CdcRegister::Index: { // status register
    uint8_t st = (uint8_t)index;
    if (param_n == 0) {
      st |= 0x08; // PRMEMPT (param FIFO empty)
    }
    if (param_n < 16) {
      st |= 0x10; // PRMWRDY (param FIFO not full)
    }
    if (resp_rd < resultsEntry()->len) {
      st |= 0x20; // RSLRRDY (response ready)
    }
    if (bfrd && data_rd < data_n) {
      st |= 0x40; // DRQSTS (data FIFO not empty)
    }
    if (command_event_armed && command_phase <= 1) {
      st |= 0x80; // BUSYSTS while command receive/argument/execution is pending
    }
    return st;
  }
  case CdcRegister::Response: { // response FIFO
    const CdcIrqEnt *f = resultsEntry();
    return resp_rd < f->len ? f->resp[resp_rd++] : 0;
  }
  case CdcRegister::Parameter: { // data FIFO, popped exactly as DMA3 pops it
    if (!bfrd || data_rd >= data_n) {
      return 0;
    }
    const uint8_t b = data[data_rd++];
    if (data_rd >= data_n) {
      sectorConsumed();
    }
    return b;
  }
  default: // 3: bank0/2 interrupt enable; bank1/3 interrupt flag (low 3 bits = pending type)
    if (index & 1) {
      return queueEmpty() ? 0xE0 : (uint8_t)(0xE0 | q[q_head].type);
    }
    return irq_en | 0xE0;
  }
}

void CdController::write(CdcRegister reg, uint8_t v) {
  switch (reg) {
  case CdcRegister::Index:
    index = v & 3;
    return; // index/bank select
  case CdcRegister::Response:
    if (index == 0) {
      scheduleCommand(v); // command register
    }
    return;
  case CdcRegister::Parameter:
    if (index == 0) {
      if (param_n < 16) {
        param[param_n++] = v;
      }
    } // param FIFO push
    else if (index == 1) {
      // Unmasking a current response's type is a fresh edge.
      const bool line_was_asserted = irqLine();
      irq_en = v;
      if (!line_was_asserted && irqLine()) {
        irq_edge = 1;
      }
    }
    return;
  default:            // 3
    if (index == 1) { // interrupt flag: write 1s to ack/clear
      if (v & 0x07) { // ack current IRQ -> advance the queue
        if (!queueEmpty()) {
          q_head = (q_head + 1) & 7;
          // The response queued behind it becomes current: a fresh interrupt.
          if (!queueEmpty()) {
            resp_rd = 0; // its results replace the acknowledged ones
            responseBecameCurrent();
          }
        }
      }
      if (v & 0x40) {
        param_n = 0; // reset param FIFO
      }
    } else if (index == 0) { // request register
      writeRequestRegister(v);
    }
    return;
  }
}

} // namespace psx::cd

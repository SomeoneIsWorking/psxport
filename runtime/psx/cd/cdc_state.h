// cdc_state.h — psx::cd::CdController: the CXD1199-style register model at 0x1F801800-3 (index/bank,
// param/response/data FIFOs, the pending-interrupt queue, drive/read state), one per Game. Its registers
// stay public members because machine state saves them field by field (state/device_bus.cpp).
#pragma once
#include <stdint.h>

namespace psx::audio {
class XaStream;
}

namespace psx::cd {

class Disc;

struct CdcIrqEnt {
  uint8_t type;
  uint8_t resp[16];
  int len;
};

using CdcTickNowFn = uint64_t (*)(void *context);
using CdcDiscReadRawFn = bool (*)(Disc *, uint32_t, uint8_t *, uint32_t);
using CdcDiscReadSectorFn = bool (*)(Disc *, uint32_t, uint8_t *);
using CdcDiscGetSubqPositionFn = bool (*)(Disc *, uint32_t, uint8_t *);

// Register ports within 0x1F801800 (bank 0 names; IrqFlag is port 3 in bank 1).
enum class CdcRegister : uint8_t { Index = 0, Response = 1, Parameter = 2, Request = 3, IrqFlag = 3 };

// The port an access to 0x1F801800..0x1F80180F reaches; Beetle mirrors ports 0..3 above 0x1F801803.
inline constexpr CdcRegister cdcRegister(uint32_t address) {
  return static_cast<CdcRegister>(address & 3u);
}

// Bank-select values and the status bit that says a response byte is available.
inline constexpr uint8_t kCdcBankData = 0u;
inline constexpr uint8_t kCdcBankIrq = 1u;
inline constexpr uint32_t kCdcStatResponseReady = 0x20u;

// Where a raw sector goes during a continuous read (vendor cdc.c DS_READING arm).
enum class CdcSectorRoute : uint8_t { Data, XaAudio, XaDropped };
enum class CdcCommandEvent : uint8_t { None, Execute, Complete };

class CdController {
public:
  // Power-on defaults (stat=0x02); keeps the disc and XA wiring.
  void reset();
  // Inject the deterministic guest-instruction clock; tests bind a fake counter.
  void bindTickSource(void *context, CdcTickNowFn now);
  // Service due drive and command events. True only when a response became current; it raised an interrupt edge
  // only if the interrupt-enable register admits its type.
  bool service();
  // Absolute tick of the earliest armed drive, command or paced read-completion deadline; false when none is armed.
  bool nextDeadlineTicks(uint64_t *absoluteTicks) const;
  // The eight GetlocP bytes for the head's current sector; false when the disc layout is unavailable.
  bool getlocP(uint8_t out[8]) const;
  // The current response type without consuming it; 0 when the queue is empty.
  uint8_t currentIrqType() const;
  // Queue one INT1 data-ready with the current status, for a completion the native layer owes the guest. False,
  // queuing nothing, when the response queue is full.
  bool postDataReady();
  // The same completion, announced once the drive would have read `sectors` from `first_lba` at `mode`'s speed
  // (seek, spin-up, one period per sector; reads queue serially). False when `sectors` is 0 or four are owed. With
  // no tick source bound it posts at once.
  bool postDataReadyAfterRead(uint32_t first_lba, uint32_t sectors, uint8_t mode);
  uint32_t read(CdcRegister reg);
  void write(CdcRegister reg, uint8_t v);
  // DMA3 read: FIFO data first, then zeros after depletion. Returns the FIFO-sourced word count.
  int dmaRead(uint32_t *out, int words);
  // Position at `lba` and start a continuous read, so a guest that drives the hardware directly sees real data.
  void beginRead(uint32_t lba);
  // Issue one parameterless command the native CD layer intercepted (Pause, Stop), with both its responses.
  void issueCommand(uint8_t command);
  // Drain the current response, keeping the first `capacity` bytes in `bytes`. Returns how many it held.
  int takeCurrentResponse(uint8_t *bytes, int capacity);
  // Mirror an intercepted Setmode; bit 0x20 decides whole-sector framing.
  void setMode(uint8_t mode);
  // Mirror Setfilter; MODE_SF routes only this XA file/channel to the SPU.
  void setFilter(uint8_t file, uint8_t channel);
  CdcSectorRoute sectorRoute(const uint8_t *raw) const;

  int index = 0;          // 0x1F801800 low 2 bits (register bank)
  uint8_t param[16] = {}; // param FIFO
  int param_n = 0;
  uint8_t data[2340] = {}; // sector data FIFO
  int data_n = 0;
  int data_rd = 0;
  uint8_t irq_en = 0;       // interrupt enable register; gates irq_edge
  uint8_t stat = 0;         // drive status byte (bit1 = motor on)
  uint32_t loc_lba = 0;     // current physical sector
  uint32_t command_lba = 0; // Setloc target, applied by ReadN/ReadS/Seek
  uint8_t mode = 0;
  uint8_t filter_file = 0;
  uint8_t filter_chan = 0;
  int reading = 0;                    // ReadN/ReadS active
  uint8_t first_sector_pending = 0;   // ReadN accepted; first data sector waits for its drive deadline
  uint8_t bfrd = 0;                   // request-register BFRD latch (bit 7)
  uint8_t following_sector_ready = 0; // drive-side next-sector availability, independent of FIFO drain
  uint8_t drive_event_armed = 0;
  uint64_t drive_deadline_ticks = 0; // absolute, in the injected guest-instruction domain
  uint8_t command_event_armed = 0;
  uint8_t pending_command = 0;
  int8_t command_phase = 0; // -1 receive start, 0 arguments, 1 execute, 2 complete
  uint8_t command_arg_latch = 0;
  uint8_t command_args[16] = {};
  uint8_t command_arg_n = 0;
  uint64_t command_deadline_ticks = 0;
  uint8_t read_completion_n = 0;                   // owed read completions not yet announced (<= 4)
  uint64_t read_completion_deadline_ticks[4] = {}; // oldest first
  void *tick_context = nullptr;
  CdcTickNowFn tick_now = nullptr;
  CdcIrqEnt q[8] = {}; // pending-interrupt queue
  int q_head = 0;
  int q_tail = 0;
  int resp_rd = 0;           // results read position in the current or last-acknowledged response
  uint64_t irq_sequence = 0; // increments whenever a response becomes current
  // The controller just raised an interrupt; the MMIO dispatcher latches it into I_STAT bit 2 (edge-triggered).
  uint8_t irq_edge = 0;
  Disc *disc = nullptr;               // Game-owned disc backend
  psx::audio::XaStream *xa = nullptr; // Game-owned XA ring the drive decodes STRSND audio into
  // The sector source; reset() points these at Disc so tests can inject fake sectors.
  CdcDiscReadRawFn disc_read_raw_fn = nullptr;
  CdcDiscReadSectorFn disc_read_sector_fn = nullptr;
  CdcDiscGetSubqPositionFn disc_get_subq_position_fn = nullptr;

private:
  enum class ReadStop : uint8_t { GuestCommand, DriveStopped };

  bool queueEmpty() const;
  bool irqLine() const;
  void responseBecameCurrent();
  void raise(uint8_t type, const uint8_t *resp, int len);
  const CdcIrqEnt *resultsEntry() const;
  uint32_t commandArgsLba() const;
  uint64_t seekTime(uint32_t target_lba) const;
  int routeAudioToSpu(const uint8_t *raw, uint32_t drive_lba);
  bool consumeSector(uint32_t lba, bool *out_audio);
  bool loadSector(uint32_t lba);
  void queueDataReady();
  bool serviceReadCompletions();
  void cancelDriveEvent();
  void scheduleSectorEventAfter(uint64_t origin);
  void scheduleSectorEvent();
  bool stopContinuousRead(ReadStop stop);
  void startContinuousRead();
  bool serviceDueSector(uint64_t now_ticks);
  bool serviceDriveEvent();
  void sectorConsumed();
  void retirePendingCommand();
  uint64_t execCommand(uint8_t cmd);
  void completeCommand();
  uint32_t sectorPayloadBytes() const;
  void writeRequestRegister(uint8_t value);
  // cdc_command_phase.cpp: oracle-derived receive/argument/execute timing.
  uint8_t popParameter();
  void scheduleCommand(uint8_t command);
  CdcCommandEvent serviceCommand(bool completion_can_raise_irq);
  void finishCommandExecution(uint64_t completion_delay_ticks);
};

} // namespace psx::cd

// device_bus.cpp — the devices between the CPU and the disc: the CD controller, the display clock,
// the DMA controller, the BIOS HLE/interrupt state, the controller port with the pad behind it, the
// memory card, and the native CD subsystem.
//
// WHAT IS NOT HERE, named so it cannot be mistaken for an oversight:
//
//   * CdcState's function pointers, tick context, disc and XA back-pointers. Those are OWNERSHIP
//     (which Game's disc backend this controller reads from), restored by Game's own construction,
//     not machine state; serializing a pointer would write an address into a file.
//   * Memcard's host FILE* and path. Also ownership. Its CONTENTS are captured — see writeCardSection
//     — because the card is an input the guest reads and a file the run writes, so a run restored
//     against a different card is a different machine.
//   * The Pad's SDL handles and its recording/replay session index. Host-side input plumbing.
//   * Cd's pending native read cursors beyond the position/setloc state, which live in CdcState.
#include "device_sections.h"

#include "cd.h"
#include "cdc_state.h"
#include "core.h"
#include "dma_irq.h"
#include "emulated_time.h"
#include "game.h"
#include "hle.h"
#include "memcard.h"
#include "sio_pad.h"
#include "timing.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace psx::state {
namespace {

constexpr std::uint8_t kBusLayoutVersion = 2;

// CdcState's queue, verbatim. `CdcIrqEnt` is a plain-C struct so it is written field by field rather
// than memcpy'd: a memcpy would put the struct's padding in the file, and padding is not a contract.
void writeIrqQueue(const CdcState &cdc, BlobWriter &out) {
  out.i32(cdc.q_head);
  out.i32(cdc.q_tail);
  out.i32(cdc.resp_rd);
  out.u64(cdc.irq_sequence);
  out.u8(cdc.irq_edge);
  for (const CdcIrqEnt &entry : cdc.q) {
    out.u8(entry.type);
    out.array(entry.resp, std::size(entry.resp));
    out.i32(entry.len);
  }
}

void readIrqQueue(CdcState &cdc, BlobReader &in) {
  cdc.q_head = in.i32();
  cdc.q_tail = in.i32();
  cdc.resp_rd = in.i32();
  cdc.irq_sequence = in.u64();
  cdc.irq_edge = in.u8();
  for (CdcIrqEnt &entry : cdc.q) {
    entry.type = in.u8();
    in.array(entry.resp, std::size(entry.resp));
    entry.len = in.i32();
  }
}

} // namespace

void writeCdcSection(Game &game, BlobWriter &out) {
  const CdcState &cdc = game.cdc;
  out.u8(kBusLayoutVersion);
  out.i32(cdc.index);
  out.array(cdc.param, std::size(cdc.param));
  out.i32(cdc.param_n);
  out.array(cdc.data, std::size(cdc.data));
  out.i32(cdc.data_n);
  out.i32(cdc.data_rd);
  out.u8(cdc.irq_en);
  out.u8(cdc.stat);
  out.u32(cdc.loc_lba);
  out.u32(cdc.command_lba);
  out.u8(cdc.mode);
  out.u8(cdc.filter_file);
  out.u8(cdc.filter_chan);
  out.i32(cdc.reading);
  out.u8(cdc.first_sector_pending);
  out.u8(cdc.bfrd);
  out.u8(cdc.following_sector_ready);
  out.u8(cdc.drive_event_armed);
  out.u64(cdc.drive_deadline_ticks);
  out.u8(cdc.command_event_armed);
  out.u8(cdc.pending_command);
  out.i8(cdc.command_phase);
  out.u8(cdc.command_arg_latch);
  out.array(cdc.command_args, std::size(cdc.command_args));
  out.u8(cdc.command_arg_n);
  out.u64(cdc.command_deadline_ticks);
  out.u8(cdc.read_completion_n);
  out.array(cdc.read_completion_deadline_ticks, std::size(cdc.read_completion_deadline_ticks));
  writeIrqQueue(cdc, out);
}

bool readCdcSection(Game &game, BlobReader &in, std::string &error) {
  const std::uint8_t layout = in.u8();
  if (layout != kBusLayoutVersion) {
    error = "cdc section layout " + std::to_string(layout) + ", this build writes " + std::to_string(kBusLayoutVersion);
    return false;
  }
  CdcState &cdc = game.cdc;
  cdc.index = in.i32();
  in.array(cdc.param, std::size(cdc.param));
  cdc.param_n = in.i32();
  in.array(cdc.data, std::size(cdc.data));
  cdc.data_n = in.i32();
  cdc.data_rd = in.i32();
  cdc.irq_en = in.u8();
  cdc.stat = in.u8();
  cdc.loc_lba = in.u32();
  cdc.command_lba = in.u32();
  cdc.mode = in.u8();
  cdc.filter_file = in.u8();
  cdc.filter_chan = in.u8();
  cdc.reading = in.i32();
  cdc.first_sector_pending = in.u8();
  cdc.bfrd = in.u8();
  cdc.following_sector_ready = in.u8();
  cdc.drive_event_armed = in.u8();
  cdc.drive_deadline_ticks = in.u64();
  cdc.command_event_armed = in.u8();
  cdc.pending_command = in.u8();
  cdc.command_phase = in.i8();
  cdc.command_arg_latch = in.u8();
  in.array(cdc.command_args, std::size(cdc.command_args));
  cdc.command_arg_n = in.u8();
  cdc.command_deadline_ticks = in.u64();
  cdc.read_completion_n = in.u8();
  in.array(cdc.read_completion_deadline_ticks, std::size(cdc.read_completion_deadline_ticks));
  readIrqQueue(cdc, in);
  if (!in.ok()) {
    error = "the cdc section is " + std::to_string(in.size()) + " bytes and did not decode (" +
            std::to_string(in.offset()) + " consumed)";
    return false;
  }
  if (cdc.q_head < 0 || cdc.q_head >= static_cast<int>(std::size(cdc.q)) || cdc.q_tail < 0 ||
      cdc.q_tail >= static_cast<int>(std::size(cdc.q))) {
    error = "the cdc section carries queue cursors (" + std::to_string(cdc.q_head) + "," + std::to_string(cdc.q_tail) +
            ") outside the 0.." + std::to_string(std::size(cdc.q) - 1) + " ring";
    return false;
  }
  if (cdc.read_completion_n > 4) {
    error = "the cdc section owes " + std::to_string(static_cast<int>(cdc.read_completion_n)) +
            " read completions; the model holds 4";
    return false;
  }
  return true;
}

void writeTimingSection(Game &game, BlobWriter &out) {
  const psx::frame::Timing &timing = game.timing;
  out.u8(kBusLayoutVersion);
  out.u32(timing.vblank);
  out.u32(timing.logicFrame);
  out.u64(timing.guestInstructionTicks);
  out.u32(timing.rootCounter2Mode);
  out.u32(timing.rootCounter2Target);
  out.u16(timing.rootCounter2BaseValue);
  out.u64(timing.rootCounter2OriginTicks);
  out.u64(timing.rootCounter1Offset);
  const psx::frame::Timing::ClockSnapshot clock = timing.clockSnapshot();
  out.u64(clock.nowQ32);
  out.u64(clock.displayBoundaryQ32);
  out.u64(clock.displayPhaseNumerator);
  out.u64(clock.displayPhaseDenominator);
}

bool readTimingSection(Game &game, BlobReader &in, std::string &error) {
  const std::uint8_t layout = in.u8();
  if (layout != kBusLayoutVersion) {
    error =
        "timing section layout " + std::to_string(layout) + ", this build writes " + std::to_string(kBusLayoutVersion);
    return false;
  }
  psx::frame::Timing &timing = game.timing;
  const std::uint32_t vblank = in.u32();
  const std::uint32_t logicFrame = in.u32();
  const std::uint64_t ticks = in.u64();
  const std::uint32_t mode = in.u32();
  const std::uint32_t target = in.u32();
  const std::uint16_t base = in.u16();
  const std::uint64_t origin = in.u64();
  const std::uint64_t counter1Offset = in.u64();
  psx::frame::Timing::ClockSnapshot clock{};
  clock.nowQ32 = in.u64();
  clock.displayBoundaryQ32 = in.u64();
  clock.displayPhaseNumerator = in.u64();
  clock.displayPhaseDenominator = in.u64();
  if (!in.ok()) {
    error = "the timing section is " + std::to_string(in.size()) + " bytes and did not decode (" +
            std::to_string(in.offset()) + " consumed)";
    return false;
  }
  if (clock.displayPhaseDenominator == 0) {
    error = "the timing section carries a zero display-phase denominator, which would divide by zero "
            "on the next pacing call";
    return false;
  }
  timing.restoreClockSnapshot(clock);
  timing.vblank = vblank;
  timing.logicFrame = logicFrame;
  timing.guestInstructionTicks = ticks;
  timing.rootCounter2Mode = mode;
  timing.rootCounter2Target = target;
  timing.rootCounter2BaseValue = base;
  timing.rootCounter2OriginTicks = origin;
  timing.rootCounter1Offset = counter1Offset;
  return true;
}

void writeDmaSection(Core &core, BlobWriter &out) {
  const DmaRegisters &dma = core.dma;
  out.u8(kBusLayoutVersion);
  out.u32(dma.dma3Madr);
  out.u32(dma.dma3Bcr);
  out.u32(dma.dma3Chcr);
  out.u32(dma.dpcr);
  out.u32(dma.dicr);
  out.u32(dma.done.mask);
}

bool readDmaSection(Core &core, BlobReader &in, std::string &error) {
  const std::uint8_t layout = in.u8();
  if (layout != kBusLayoutVersion) {
    error = "dma section layout " + std::to_string(layout) + ", this build writes " + std::to_string(kBusLayoutVersion);
    return false;
  }
  DmaRegisters &dma = core.dma;
  dma.dma3Madr = in.u32();
  dma.dma3Bcr = in.u32();
  dma.dma3Chcr = in.u32();
  dma.dpcr = in.u32();
  dma.dicr = in.u32();
  dma.done.mask = in.u32();
  if (!in.ok()) {
    error = "the dma section is " + std::to_string(in.size()) + " bytes and did not decode (" +
            std::to_string(in.offset()) + " consumed)";
    return false;
  }
  return true;
}

void writeHleSection(Game &game, BlobWriter &out) {
  const Hle &hle = game.hle;
  out.u8(kBusLayoutVersion);
  for (const HleEvCB &slot : hle.ev) {
    out.i32(slot.open);
    out.i32(slot.enabled);
    out.i32(slot.fired);
    out.u32(slot.ev_class);
    out.u32(slot.spec);
    out.u32(slot.mode);
    out.u32(slot.func);
  }
  out.u32(hle.i_stat);
  out.u32(hle.i_mask);
  out.i32(hle.irq_n);
  out.array(hle.irq_elem, std::size(hle.irq_elem));
  out.array(hle.irq_prio, std::size(hle.irq_prio));
  out.i32(hle.in_irq);
  out.i32(hle.ev_depth);
  out.u32(hle.exception_exit_buf);
  out.i32(hle.irq_enabled);

  // The native heap: block addresses and sizes live in GUEST memory and are re-read through the
  // guest address space, but the allocation bitmap and the cursor do not, so a restored run would
  // hand the same guest address to two owners or reuse one that is still held.
  out.u32(hle.heap_base);
  out.u32(hle.heap_size);
  out.i32(hle.heap_ok);
  out.i32(hle.nblk);
  for (const HleHeapBlock &block : hle.blk) {
    out.u32(block.addr);
    out.u32(block.size);
    out.i32(block.used);
  }
  out.u32(hle.rand_state);
  out.i32(hle.work_ok);
  out.boolean(hle.bios_pad_initialized);
  out.boolean(hle.bios_pad_irq_started);
  out.i32(hle.dcb_n);
}

bool readHleSection(Game &game, BlobReader &in, std::string &error) {
  const std::uint8_t layout = in.u8();
  if (layout != kBusLayoutVersion) {
    error = "hle section layout " + std::to_string(layout) + ", this build writes " + std::to_string(kBusLayoutVersion);
    return false;
  }
  Hle &hle = game.hle;
  std::vector<HleEvCB> events(std::size(hle.ev));
  for (HleEvCB &slot : events) {
    slot.open = in.i32();
    slot.enabled = in.i32();
    slot.fired = in.i32();
    slot.ev_class = in.u32();
    slot.spec = in.u32();
    slot.mode = in.u32();
    slot.func = in.u32();
  }
  const std::uint32_t iStat = in.u32();
  const std::uint32_t iMask = in.u32();
  const std::int32_t irqN = in.i32();
  std::array<std::uint32_t, Hle::IRQ_CHAIN_MAX> elem{};
  std::array<std::uint32_t, Hle::IRQ_CHAIN_MAX> prio{};
  in.array(elem.data(), elem.size());
  in.array(prio.data(), prio.size());
  const std::int32_t inIrq = in.i32();
  const std::int32_t evDepth = in.i32();
  const std::uint32_t exitBuf = in.u32();
  const std::int32_t irqEnabled = in.i32();

  const std::uint32_t heapBase = in.u32();
  const std::uint32_t heapSize = in.u32();
  const std::int32_t heapOk = in.i32();
  const std::int32_t nblk = in.i32();
  std::vector<HleHeapBlock> blocks(std::size(hle.blk));
  for (HleHeapBlock &block : blocks) {
    block.addr = in.u32();
    block.size = in.u32();
    block.used = in.i32();
  }
  const std::uint32_t randState = in.u32();
  const std::int32_t workOk = in.i32();
  const bool padInitialized = in.boolean();
  const bool padIrqStarted = in.boolean();
  const std::int32_t dcbN = in.i32();
  if (!in.ok()) {
    error = "the hle section is " + std::to_string(in.size()) + " bytes and did not decode (" +
            std::to_string(in.offset()) + " consumed)";
    return false;
  }
  if (irqN < 0 || irqN > Hle::IRQ_CHAIN_MAX) {
    error = "the hle section carries " + std::to_string(irqN) +
            " registered interrupt elements, "
            "outside 0.." +
            std::to_string(Hle::IRQ_CHAIN_MAX);
    return false;
  }
  if (nblk < 0 || nblk > static_cast<std::int32_t>(std::size(hle.blk))) {
    error = "the hle section carries " + std::to_string(nblk) + " heap blocks, outside 0.." +
            std::to_string(std::size(hle.blk));
    return false;
  }

  // Everything decoded and every bound checked: only now is the machine mutated. A section that
  // fails half way through leaves the interrupt controller, the heap bitmap and the event table
  // exactly as they were, which is the only way a refused load can be safe to retry.
  std::copy(events.begin(), events.end(), std::begin(hle.ev));
  hle.i_stat = iStat;
  hle.i_mask = iMask;
  hle.irq_n = irqN;
  std::copy(elem.begin(), elem.end(), std::begin(hle.irq_elem));
  std::copy(prio.begin(), prio.end(), std::begin(hle.irq_prio));
  hle.in_irq = inIrq;
  hle.ev_depth = evDepth;
  hle.exception_exit_buf = exitBuf;
  hle.irq_enabled = irqEnabled;
  hle.heap_base = heapBase;
  hle.heap_size = heapSize;
  hle.heap_ok = heapOk;
  hle.nblk = nblk;
  std::copy(blocks.begin(), blocks.end(), std::begin(hle.blk));
  hle.rand_state = randState;
  hle.work_ok = workOk;
  hle.bios_pad_initialized = padInitialized;
  hle.bios_pad_irq_started = padIrqStarted;
  hle.dcb_n = dcbN;
  return true;
}

void writeSioSection(Game &game, BlobWriter &out) {
  const Sio0 &sio = game.sio;
  const Pad &pad = game.pad;
  out.u8(kBusLayoutVersion);
  out.u16(sio.mode);
  out.u16(sio.ctrl);
  out.u16(sio.baud);
  out.i32(sio.rx);
  out.i32(sio.pos);
  out.boolean(sio.irq);
  out.u64(sio.rxReadyTicks);
  out.u64(sio.ackTicks);
  out.u64(sio.ackPulseEndTicks);
  out.u16(pad.buttons);
  out.u16(pad.repl_hold);
  out.u16(pad.repl_tap);
  out.i32(pad.repl_tap_n);
  out.i32(pad.repl_on);
}

bool readSioSection(Game &game, BlobReader &in, std::string &error) {
  const std::uint8_t layout = in.u8();
  if (layout != kBusLayoutVersion) {
    error = "sio section layout " + std::to_string(layout) + ", this build writes " + std::to_string(kBusLayoutVersion);
    return false;
  }
  Sio0 &sio = game.sio;
  const std::uint16_t mode = in.u16();
  const std::uint16_t ctrl = in.u16();
  const std::uint16_t baud = in.u16();
  const std::int32_t rx = in.i32();
  const std::int32_t pos = in.i32();
  const bool irq = in.boolean();
  const std::uint64_t rxReady = in.u64();
  const std::uint64_t ack = in.u64();
  const std::uint64_t ackPulseEnd = in.u64();
  Pad &pad = game.pad;
  const std::uint16_t buttons = in.u16();
  const std::uint16_t replHold = in.u16();
  const std::uint16_t replTap = in.u16();
  const std::int32_t replTapN = in.i32();
  const std::int32_t replOn = in.i32();
  if (!in.ok()) {
    error = "the sio section is " + std::to_string(in.size()) + " bytes and did not decode (" +
            std::to_string(in.offset()) + " consumed)";
    return false;
  }
  sio.mode = mode;
  sio.ctrl = ctrl;
  sio.baud = baud;
  sio.rx = rx;
  sio.pos = pos;
  sio.irq = irq;
  sio.rxReadyTicks = rxReady;
  sio.ackTicks = ack;
  sio.ackPulseEndTicks = ackPulseEnd;
  pad.buttons = buttons;
  pad.repl_hold = replHold;
  pad.repl_tap = replTap;
  pad.repl_tap_n = replTapN;
  pad.repl_on = replOn;
  // The button EDGE sampler is re-seeded from the restored mask rather than carrying its own state
  // across: an edge is a CHANGE, and restoring a stale "pressed" bit would make the first frame of
  // the loaded run invent a release the guest never saw.
  pad.resetButtonEdges(buttons);
  return true;
}

void writeCardSection(Game &game, BlobWriter &out, std::string &error) {
  out.u8(kBusLayoutVersion);
  if (!game.memcard.present()) {
    error =
        "the memory card is not open (" + std::string(game.memcard.path()) + "), so this run has no card state to save";
    return;
  }
  std::vector<std::uint8_t> card(Memcard::kSize, 0);
  for (std::uint32_t frame = 0; frame < Memcard::kFrames; ++frame) {
    if (!game.memcard.readFrame(frame, card.data() + frame * Memcard::kFrameSize)) {
      error = "memory card frame " + std::to_string(frame) + " of " + std::to_string(Memcard::kFrames) +
              " could not be read from " + game.memcard.path();
      return;
    }
  }
  // Length-prefixed, so an EMPTY card image and a MISSING one cannot be confused on load.
  out.blob(card);
  // Open guest file descriptors: the BIOS file API hands the guest descriptor numbers, so a
  // restored run that kept the RAM but not the descriptor table would call the guest's next read
  // against the wrong card handle.
  out.u32(static_cast<std::uint32_t>(Memcard::kFdMax));
  for (int fd = 0; fd < Memcard::kFdMax; ++fd) {
    const McFd *entry = game.memcard.descriptorAt(fd);
    out.boolean(entry != nullptr && entry->used != 0);
    out.i32(entry != nullptr ? entry->block : 0);
    out.u32(entry != nullptr ? entry->pos : 0u);
    out.u32(entry != nullptr ? entry->size : 0u);
  }
  // The directory-enumeration cursor: a guest caught mid-`nextfile` continues from here.
  out.u32(static_cast<std::uint32_t>(std::strlen(game.memcard.scanPattern())) + 1u);
  out.bytes(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(game.memcard.scanPattern()),
                                          std::strlen(game.memcard.scanPattern()) + 1u));
  out.u32(game.memcard.scanBlock());
}

bool readCardSection(Game &game, BlobReader &in, std::string &error) {
  const std::uint8_t layout = in.u8();
  if (layout != kBusLayoutVersion) {
    error =
        "card section layout " + std::to_string(layout) + ", this build writes " + std::to_string(kBusLayoutVersion);
    return false;
  }
  if (!game.memcard.present()) {
    error = "the memory card is not open (" + std::string(game.memcard.path()) +
            "), so a state carrying card contents cannot be loaded";
    return false;
  }
  std::vector<std::uint8_t> card;
  if (!in.blob(card) || card.size() != Memcard::kSize) {
    error = "the card section carries " + std::to_string(card.size()) + " bytes of card image; " +
            std::to_string(Memcard::kSize) + " were needed";
    return false;
  }
  const std::uint32_t fdCount = in.u32();
  if (!in.ok() || fdCount != static_cast<std::uint32_t>(Memcard::kFdMax)) {
    error = "the card section declares " + std::to_string(fdCount) + " descriptors; this build has " +
            std::to_string(Memcard::kFdMax);
    return false;
  }
  // Written to the card only after every frame has been read back successfully, so a card write that
  // fails half way cannot leave the host file describing a machine that never existed. The card is
  // a host file the run OWNS, so this is the one place a load writes outside the state file itself.
  std::uint32_t written = 0;
  for (; written < Memcard::kFrames; ++written) {
    if (!game.memcard.writeFrame(written, card.data() + written * Memcard::kFrameSize)) {
      error = "memory card frame " + std::to_string(written) + " of " + std::to_string(Memcard::kFrames) +
              " could not be written to " + game.memcard.path();
      return false;
    }
  }
  for (int fd = 0; fd < Memcard::kFdMax; ++fd) {
    const bool used = in.boolean();
    const std::int32_t block = in.i32();
    const std::uint32_t pos = in.u32();
    const std::uint32_t size = in.u32();
    if (!in.ok()) {
      error = "the card section's descriptor table did not decode";
      return false;
    }
    McFd entry{};
    entry.used = used ? 1 : 0;
    entry.block = block;
    entry.pos = pos;
    entry.size = size;
    game.memcard.restoreDescriptor(fd, entry);
  }
  const std::uint32_t patternLength = in.u32();
  if (!in.ok() || patternLength == 0 || patternLength > 64) {
    error = "the card section's enumeration cursor names a " + std::to_string(patternLength) +
            "-byte pattern; this build's slot is 64";
    return false;
  }
  std::array<char, 64> pattern{};
  if (!in.bytes(std::span<std::uint8_t>(reinterpret_cast<std::uint8_t *>(pattern.data()), patternLength))) {
    error = "the card section's enumeration pattern did not decode";
    return false;
  }
  const std::uint32_t scanBlock = in.u32();
  if (!in.ok()) {
    error = "the card section did not decode to its end";
    return false;
  }
  game.memcard.restoreScanCursor(pattern.data(), scanBlock);
  return true;
}

void writeCdSection(Game &game, BlobWriter &out) {
  const Cd &cd = game.cd;
  out.u8(kBusLayoutVersion);
  out.i32(cd.pending_music);
  out.u8(cd.pm_chan);
  out.u32(cd.pm_start);
  out.u32(cd.pm_end);
  out.array(cd.stock_command_response.data(), cd.stock_command_response.size());
  out.boolean(cd.stock_command_response_valid);
  out.i32(cd.setloc_lba);
  out.i32(cd.stock_reading);
  out.i32(cd.in_stock_read);
}

bool readCdSection(Game &game, BlobReader &in, std::string &error) {
  const std::uint8_t layout = in.u8();
  if (layout != kBusLayoutVersion) {
    error = "cd section layout " + std::to_string(layout) + ", this build writes " + std::to_string(kBusLayoutVersion);
    return false;
  }
  Cd &cd = game.cd;
  cd.pending_music = in.i32();
  cd.pm_chan = in.u8();
  cd.pm_start = in.u32();
  cd.pm_end = in.u32();
  in.array(cd.stock_command_response.data(), cd.stock_command_response.size());
  cd.stock_command_response_valid = in.boolean();
  cd.setloc_lba = in.i32();
  cd.stock_reading = in.i32();
  cd.in_stock_read = in.i32();
  if (!in.ok()) {
    error = "the cd section is " + std::to_string(in.size()) + " bytes and did not decode (" +
            std::to_string(in.offset()) + " consumed)";
    return false;
  }
  if (cd.in_stock_read) {
    error = "the cd section claims a native stock read is IN PROGRESS; that is only true while the "
            "guest is inside the read call, and a state cannot be taken there";
    return false;
  }
  return true;
}

} // namespace psx::state

// Core/psx::frame::Timing member definitions live at global scope, next to nothing: they are ordinary members of
// their own classes, defined here because this is where the save-state owner reaches their private
// state, and NOT because of any ownership they have over the machine.
psx::frame::Timing::ClockSnapshot psx::frame::Timing::clockSnapshot() const {
  return ClockSnapshot{mEmulatedTime.nowQ32(),
                       mEmulatedTime.displayBoundaryQ32(),
                       static_cast<std::uint64_t>(mDisplayFieldPhaseNumerator),
                       static_cast<std::uint64_t>(mDisplayFieldPhaseDenominator)};
}

void psx::frame::Timing::restoreClockSnapshot(const ClockSnapshot &clock) {
  mEmulatedTime.restoreQ32(clock.nowQ32, clock.displayBoundaryQ32);
  mDisplayFieldPhaseNumerator = static_cast<unsigned __int128>(clock.displayPhaseNumerator);
  mDisplayFieldPhaseDenominator = static_cast<unsigned __int128>(clock.displayPhaseDenominator);
}

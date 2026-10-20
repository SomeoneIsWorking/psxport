// mem.cpp — Core's guest address map, the I/O decode it routes to, and the DMA channels.
#include "cdc_state.h"
#include "core.h"
#include "dma_irq.h"
#include "dma_linked_list.h"
#include "game.h"
#include "gpu_device.h"
#include "gpu_ports.h"
#include "guest_code_module.h"
#include "guest_memory_map.h"
#include "mdec_device.h"
#include "host_backtrace.h"
#include "invalidation.h"
#include "io_peripherals.h"
#include "spu_beetle.h"

#include <lucent/log.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

using psx::cpu::gprName;
using psx::dma::DmaChannel;

std::optional<GuestAddressRange> Core::mappedMainRamRange(uint32_t address, uint32_t bytes) {
  if (bytes == 0u) {
    return std::nullopt;
  }
  const auto *mapped = host_ptr({address, bytes});
  if (!mapped) {
    return std::nullopt;
  }
  const auto ramBegin = reinterpret_cast<std::uintptr_t>(ram);
  const auto mappedAddress = reinterpret_cast<std::uintptr_t>(mapped);
  if (mappedAddress < ramBegin || mappedAddress >= ramBegin + sizeof(ram) ||
      bytes > ramBegin + sizeof(ram) - mappedAddress) {
    return std::nullopt;
  }
  const auto begin = static_cast<uint32_t>(mappedAddress - ramBegin);
  return GuestAddressRange{begin, begin + bytes};
}

// RAM is mirrored four times through the low 8 MiB; Spider-Man's stack lives in the top mirror.
uint8_t *Core::host_ptr(GuestSpan span) {
  const uint32_t p = psx::physical(span.address);
  if (const std::optional<uint32_t> offset = psx::mainRamOffset(p)) {
    return *offset + span.bytes <= psx::kMainRamBytes ? &ram[*offset] : nullptr;
  }
  if (p >= psx::kScratchpadPhysicalBase && p + span.bytes <= psx::kScratchpadPhysicalBase + psx::kScratchpadBytes) {
    return &scratch[p - psx::kScratchpadPhysicalBase];
  }
  return nullptr;
}

namespace {

// True when the guest armed this channel's completion and is now owed its callback.
bool dma_completed(psx::dma::DmaRegisters &dma, DmaChannel channel) {
  const bool owed = dma.done.complete(dma.dicr, channel);
  lucent::debug("dmairq",
                "DMA{} complete: armed={} (DICR {:08X})",
                psx::dma::channelIndex(channel),
                owed ? 1 : 0,
                psx::dma::readDicr(dma.dicr));
  return owed;
}

int dma_block_words(uint32_t bcr) { // sync-mode-1 block DMA total word count
  uint32_t bs = bcr & 0xFFFF, bc = bcr >> 16;
  return (int)(bs * (bc ? bc : 1));
}

} // namespace

// DMA0 (MDEC-in) and DMA1 (MDEC-out) stay pending around the decoder as in beetle dma.c (ChCan):
// each start, CHCR poll and MDEC port access pumps both channels forward together.
void Core::mdec_dma_pump() {
  MdecDevice &mdec = game->mdec;
  for (;;) {
    bool progress = false;

    // DMA0: MDEC_DMACanWrite needs the InFIFO empty, so a 0x10-word chunk is never dropped.
    while (dma.mdec0Left > 0 && mdec.canWrite()) {
      uint32_t chunk_buf[0x10];
      int chunk = dma.mdec0Left < 0x10 ? dma.mdec0Left : 0x10;
      for (int k = 0; k < chunk; k++) {
        chunk_buf[k] = mem_r32((dma.mdec0Addr + (uint32_t)k * 4) & 0x1FFFFCu);
      }
      int fed = mdec.dmaIn(chunk_buf, chunk);
      dma.mdec0Addr = (dma.mdec0Addr + (uint32_t)fed * 4) & 0xFFFFFFu;
      dma.mdec0Left -= fed;
      if (fed > 0) {
        progress = true;
      }
      if (fed < chunk) {
        break; // decoder refused mid-chunk: fall through to classify
      }
    }

    // DMA1: one MDEC_DMACanRead gate per block; word i lands at MADR + 4*(i + voffs), as dma.c CH_MDEC_OUT.
    while (dma.mdec1Left > 0 && mdec.canRead()) {
      int bs = (int)(dma.of(DmaChannel::MdecOut).bcr & 0xFFFF);
      if (bs == 0) {
        bs = 0x10000;
      }
      int burst = bs < dma.mdec1Left ? bs : dma.mdec1Left;
      // One executable-write notification per burst.
      uint32_t burstLow = 0xFFFFFFFFu;
      uint32_t burstHigh = 0;
      for (int k = 0; k < burst; k++) {
        uint32_t offs = 0;
        uint32_t val = mdec.dmaReadWord(&offs);
        const uint32_t dest = (dma.mdec1Addr + (offs << 2)) & 0x1FFFFCu;
        mem_w32_unnotified(dest, val);
        burstLow = dest < burstLow ? dest : burstLow;
        burstHigh = dest > burstHigh ? dest : burstHigh;
        dma.mdec1Addr = (dma.mdec1Addr + 4) & 0xFFFFFFu;
      }
      psx::cpu::notifyExecutableWrite(*this, {burstLow, burstHigh + 4u}, psx::cpu::ExecutableWriteSource::Dma);
      dma.mdec1Left -= burst;
      progress = true;
    }

    // Busy clears only once the word count is exhausted, as on hardware.
    if (dma.mdec0Left == 0 && (dma.of(DmaChannel::MdecIn).chcr & 0x01000000u)) {
      dma.of(DmaChannel::MdecIn).chcr &= ~0x01000000u;
      lucent::debug("mdecdma", "DMA0(in)  complete");
      if (dma_completed(dma, DmaChannel::MdecIn)) {
        pending_work |= PW_IRQ;
      }
    }
    if (dma.mdec1Left == 0 && (dma.of(DmaChannel::MdecOut).chcr & 0x01000000u)) {
      dma.of(DmaChannel::MdecOut).chcr &= ~0x01000000u;
      lucent::debug("mdecdma", "DMA1(out) complete");
      if (dma_completed(dma, DmaChannel::MdecOut)) {
        pending_work |= PW_IRQ;
      }
    }
    if (dma.mdec0Left == 0 && dma.mdec1Left == 0) {
      return; // nothing pending
    }
    if (progress) {
      continue;
    }

    // No progress: run the decoder to its next FIFO gate and classify where it parked.
    mdec.step();
    if ((dma.mdec0Left > 0 && mdec.canWrite()) || (dma.mdec1Left > 0 && mdec.canRead())) {
      continue; // the step freed a FIFO — keep pumping
    }

    const uint32_t st = mdec.read(MdecDevice::kControlPort);
    const int in_cmd = static_cast<int>((st >> 29) & 1); // bit 29: a decode command is consuming input
    const int out_has = !((st >> 31) & 1);               // bit 31 clear: OutFIFO non-empty

    if (dma.mdec0Left > 0 && !in_cmd && (st & 0xFFFF) == 0xFFFF) {
      // The command retired with input pending: hardware hangs the channel busy; report it once.
      if (!mdecStallReported_) {
        mdecStallReported_ = true;
        lucent::error("mdec",
                      "DMA0 in: {} word(s) pending @{:08X} but the decode command has retired "
                      "(InCommand clear, InCounter=0xFFFF) — input remainder outlives the "
                      "command; transfer cannot complete. MDEC1 status={:08X}",
                      dma.mdec0Left,
                      dma.mdec0Addr,
                      st);
      }
      return;
    }
    // Deferrals are traced on change only; a guest can poll an abandoned remainder millions of times.
    if (dma.mdec0Left > 0 && !in_cmd) {
      // No command in flight: the next MDEC command write resumes it.
      const uint32_t note = 0x01000000u | (uint32_t)dma.mdec0Left;
      if (mdecDeferNote_ != note) { // state CHANGE, not per poll — see above
        mdecDeferNote_ = note;
        lucent::debug("mdecdma", "DMA0(in)  deferred: {} word(s) pending, no decode command in flight", dma.mdec0Left);
      }
      return;
    }
    if (dma.mdec0Left > 0 && dma.mdec1Left == 0 && out_has) {
      // The decoder is blocked on output until the guest starts DMA1.
      const uint32_t note = 0x02000000u | (uint32_t)dma.mdec0Left;
      if (mdecDeferNote_ != note) { // state CHANGE, not per poll — see above
        mdecDeferNote_ = note;
        lucent::debug("mdecdma", "DMA0(in)  deferred: {} word(s) pending, decoder blocked on output", dma.mdec0Left);
      }
      return;
    }
    if (dma.mdec1Left > 0 && dma.mdec0Left == 0) {
      // Output below one burst and no input pending: the guest feeds more or its own timeout fires.
      const uint32_t note = 0x03000000u | (uint32_t)dma.mdec1Left;
      if (mdecDeferNote_ != note) { // state CHANGE, not per poll — see above
        mdecDeferNote_ = note;
        lucent::debug("mdecdma", "DMA1(out) deferred: {} word(s) pending, awaiting decoder output", dma.mdec1Left);
      }
      return;
    }
    // A genuine wedge: decoder state or DMA enable bits dropped mid-transfer.
    if (!mdecStallReported_) {
      mdecStallReported_ = true;
      lucent::error("mdec",
                    "DMA pump wedged: no progress possible. DMA0 {} word(s) pending @{:08X}, "
                    "DMA1 {} word(s) pending @{:08X}, MDEC1 status={:08X} (InCommand={} "
                    "outfifo_has_data={})",
                    dma.mdec0Left,
                    dma.mdec0Addr,
                    dma.mdec1Left,
                    dma.mdec1Addr,
                    st,
                    in_cmd,
                    out_has);
    }
    return;
  }
}

namespace {

// A RAM-space address that host_ptr did not map is a memory-model defect, not a stray peripheral poke.
bool ram_range_addr(uint32_t p) {
  return p < 0x1F000000u; // below the expansion and I/O windows
}

} // namespace

// Strings the live GPRs point at name the asset a bad pointer was looking up; only live pointers are visible.
void Core::dumpStringishRegs() {
  Core *const gc = this;
  int examined = 0, mapped = 0, printed = 0;
  for (uint32_t i = psx::cpu::kAt; i < psx::cpu::kGprCount; i++) {
    if (i == psx::cpu::kGp || i == psx::cpu::kSp || i == psx::cpu::kFp) {
      continue; // bases, not string pointers
    }
    examined++;
    const uint32_t a = gc->r[i];
    uint8_t *p = gc->host_ptr({a, 1});
    if (!p) {
      continue;
    }
    mapped++;
    // Printable and NUL-terminated, bounded by what host_ptr still maps.
    char buf[64];
    int n = 0;
    while (n < 63) {
      uint8_t *q = gc->host_ptr({a + (uint32_t)n, 1});
      if (!q) {
        break;
      }
      const uint8_t c = *q;
      if (c == 0) {
        break;
      }
      if (c < 0x20 || c > 0x7E) {
        n = 0;
        break;
      } // non-printable => not a string, reject outright
      buf[n++] = (char)c;
    }
    // Bytes for every mapped GPR, so a stale pointer is told from a strict classifier.
    if (n >= 4) {
      buf[n] = 0;
      lucent::error("mem", "  string: {}=0x{:08X} -> \"{}\"", gprName(i), a, buf);
      printed++;
    }
    char hex[16 * 3 + 1], asc[17];
    int hn = 0, an = 0;
    for (int k = 0; k < 16; k++) {
      uint8_t *q = gc->host_ptr({a + (uint32_t)k, 1});
      if (!q) {
        break;
      }
      hn += snprintf(hex + hn, sizeof hex - (size_t)hn, "%02X ", *q);
      asc[an++] = (*q >= 0x20 && *q <= 0x7E) ? (char)*q : '.';
    }
    hex[hn] = 0;
    asc[an] = 0;
    lucent::error("mem", "  bytes : {:<4}=0x{:08X}  {:<48} |{}|", gprName(i), a, hex, asc);
  }
  lucent::error("mem",
                "  strings: examined {} GPRs (excl gp/sp/fp), {} resolved into mapped RAM, "
                "{} looked like C strings. BLIND SPOT: only sees a string whose pointer is still "
                "live in a GPR here — one reached via a stack buffer or a struct field is NOT "
                "visible, so {} does not mean there was no name.",
                examined,
                mapped,
                printed,
                printed ? "this list" : "an empty list");
}
namespace {

// An unmapped RAM access cannot be honoured, so it aborts at the first one.
void warn_unmapped_ram(Core *gc, uint32_t a, uint32_t bytes, const char *how) {
  const uint32_t p = psx::physical(a);
  if (!ram_range_addr(p)) {
    return;
  }
  // The guest registers say where a bad pointer came from.
  if (gc) {
    // pc is the executor state at its last bounded exit, not the host fault site.
    lucent::error("mem",
                  "  guest: executor-pc=0x{:08X} (not necessarily the later host fault site) "
                  "ra=0x{:08X} sp=0x{:08X} gp=0x{:08X} fp=0x{:08X}",
                  gc->pc,
                  gc->r[psx::cpu::kRa],
                  gc->r[psx::cpu::kSp],
                  gc->r[psx::cpu::kGp],
                  gc->r[psx::cpu::kFp]);
    lucent::error("mem",
                  "  args : a0=0x{:08X} a1=0x{:08X} a2=0x{:08X} a3=0x{:08X}  v0=0x{:08X} v1=0x{:08X}",
                  gc->r[psx::cpu::kA0],
                  gc->r[psx::cpu::kA1],
                  gc->r[psx::cpu::kA2],
                  gc->r[psx::cpu::kA3],
                  gc->r[psx::cpu::kV0],
                  gc->r[psx::cpu::kV1]);
    lucent::error("mem",
                  "  temps: t0=0x{:08X} t1=0x{:08X} t2=0x{:08X} t3=0x{:08X}  s0=0x{:08X} s1=0x{:08X}",
                  gc->r[psx::cpu::kT0],
                  gc->r[psx::cpu::kT1],
                  gc->r[psx::cpu::kT2],
                  gc->r[psx::cpu::kT3],
                  gc->r[psx::cpu::kS0],
                  gc->r[psx::cpu::kS1]);
    gc->dumpStringishRegs();
  }
  lucent::error("mem",
                "FATAL: UNMAPPED RAM {}{} @ 0x{:08X} (phys 0x{:08X}): the memory model does not map it, "
                "so the access cannot be honoured. Backtrace:",
                how,
                bytes * 8,
                a,
                p);
  psxport::host::logBacktrace(32);
  abort();
}

} // namespace

uint32_t Core::io_read(uint32_t a, uint32_t bytes) {
  const uint32_t p = psx::physical(a);
  uint32_t peripheral = 0; // SIO0 + the root counters own their decode (io_peripherals.h)
  if (psx::platform::readIoPeripheral(*this, a, peripheral)) {
    return peripheral;
  }
  if (p == 0x1F801814) {
    return gpuDevice().status(psx::frame::CpuTicks{game->timing.emulatedCpuTicks()});
  }
  if (p >= 0x1F801800 && p <= 0x1F80180F) { // CD controller; registers 0..3 mirror through the window
    const uint32_t rv = game->cdc.read(psx::cd::cdcRegister(p));
    irqStatLatch(); // the controller may have raised during this access
    // cdcr: register 2 reads mean the guest drains the data FIFO itself.
    lucent::debug("cdcr",
                  "r[{:04X}]={:02X} bank={} {} pc={:08X} ra={:08X}",
                  (unsigned)(p & 0xFFFF),
                  (unsigned)(rv & 0xFF),
                  game->cdc.index,
                  (p & 3) == 2   ? "DATA-FIFO"
                  : (p & 3) == 1 ? "resp"
                  : (p & 3) == 0 ? "status"
                                 : "irq",
                  pc,
                  r[psx::cpu::kRa]);
    return rv;
  }
  // GPUREAD: a GP0(C0h) readback word, or the last GP1(10h) answer.
  if (p == 0x1F801810) {
    return gpuDevice().read();
  }
  if (p == 0x1F801820 || p == 0x1F801824) { // MDEC0 data / MDEC1 status
    // A guest poll is a pump point, so a status spin drives the pending MDEC DMA forward.
    if (dma.mdec0Left > 0 || dma.mdec1Left > 0) {
      mdec_dma_pump();
    }
    return game->mdec.read(p);
  }
  if (p == 0x1F801DAE) {
    return 0; // SPUSTAT: report idle/transfer-complete
  }
  if (p >= 0x1F801C00 && p <= 0x1F801FFF) {
    return psx::audio::spu::read(p); // SPU register file
  }
  if (p == 0x1F8010B0) {
    return dma.of(DmaChannel::Cdrom).madr; // DMA3 CDROM->RAM
  }
  if (p == 0x1F8010B4) {
    return dma.of(DmaChannel::Cdrom).bcr;
  }
  if (p == 0x1F8010B8) {
    return dma.of(DmaChannel::Cdrom).chcr; // busy bit already cleared by the write handler
  }
  if (p == 0x1F8010C0) {
    return dma.of(DmaChannel::Spu).madr;
  }
  if (p == 0x1F8010C4) {
    return dma.of(DmaChannel::Spu).bcr;
  }
  if (p == 0x1F8010C8) {
    return dma.of(DmaChannel::Spu).chcr;
  }
  if (p == 0x1F801080) {
    return dma.of(DmaChannel::MdecIn).madr;
  }
  if (p == 0x1F801084) {
    return dma.of(DmaChannel::MdecIn).bcr;
  }
  if (p == 0x1F801088) { // DMA0 CHCR: bit 24 stays SET while input is pending
    if (dma.mdec0Left > 0 || dma.mdec1Left > 0) {
      mdec_dma_pump(); // a busy-poll drives progress
    }
    return dma.of(DmaChannel::MdecIn).chcr;
  }
  if (p == 0x1F801090) {
    return dma.of(DmaChannel::MdecOut).madr;
  }
  if (p == 0x1F801094) {
    return dma.of(DmaChannel::MdecOut).bcr;
  }
  if (p == 0x1F801098) { // DMA1 CHCR: bit 24 stays SET while output is pending
    if (dma.mdec0Left > 0 || dma.mdec1Left > 0) {
      mdec_dma_pump();
    }
    return dma.of(DmaChannel::MdecOut).chcr;
  }
  if (p == 0x1F8010A0) {
    return dma.of(DmaChannel::Gpu).madr; // DMA2 MADR
  }
  if (p == 0x1F8010A4) {
    return dma.of(DmaChannel::Gpu).bcr; // DMA2 BCR
  }
  if (p == 0x1F8010A8) {
    return dma.of(DmaChannel::Gpu).chcr; // DMA2 CHCR (busy bit already cleared)
  }
  if (p >= 0x1F8010F0 && p <= 0x1F8010F3) { // DPCR — plain storage, byte-addressable
    return dma.dpcr >> ((p & 3u) * 8u);
  }
  if (p >= 0x1F8010F4 && p <= 0x1F8010F7) { // DICR — bit 31 is computed, not stored
    return psx::dma::readDicr(dma.dicr) >> ((p & 3u) * 8u);
  }
  if (p == 0x1F8010E0) {
    return dma.of(DmaChannel::Otc).madr; // DMA6 OTC MADR
  }
  if (p == 0x1F8010E4) {
    return dma.of(DmaChannel::Otc).bcr; // DMA6 OTC BCR
  }
  if (p == 0x1F8010E8) {
    return dma.of(DmaChannel::Otc).chcr; // DMA6 OTC CHCR (busy bit already cleared)
  }
  warn_unmapped_ram(this, a, bytes, "read");
  // An unmapped peripheral read returns 0, as an unimplemented register does; ra names the polling routine.
  lucent::debug("io", "read{} @ 0x{:08X} -> 0 (ra={:08X} pc={:08X})", bytes * 8, a, r[psx::cpu::kRa], pc);
  return 0;
}

void Core::io_write(uint32_t a, uint32_t v, uint32_t bytes) {
  const uint32_t p = psx::physical(a);
  if (psx::platform::writeIoPeripheral(*this, {a, v, bytes})) { // SIO0 + root counter 2 (io_peripherals.h)
    return;
  }
  if (p >= 0x1F801800 && p <= 0x1F80180F) { // CD controller (mirrored window, as io_read)
    // cdcw names the guest site of each CD register write, logged before the bank changes.
    lucent::debug("cdcw",
                  "w[{:04X}]={:02X} bank={} a0={:08X} s1={:08X} pc={:08X} ra={:08X}",
                  (unsigned)(p & 0xFFFF),
                  (unsigned)(v & 0xFF),
                  game->cdc.index,
                  r[psx::cpu::kA0],
                  r[psx::cpu::kS1],
                  pc,
                  r[psx::cpu::kRa]);
    // cdcbt adds a host backtrace to a command-register cdcw line.
    if ((p & 3) == 1 && game->cdc.index == 0 && // command-register write only
        lucent::channel_on("cdcbt") && lucent::channel_on("cdcw")) {
      psxport::host::logBacktrace(24);
    }
    game->cdc.write(psx::cd::cdcRegister(p), (uint8_t)v);
    irqStatLatch(); // an ACK can make a queued response current and raise a fresh edge
    return;
  }
  if (p == 0x1F801810) {
    gpuDevice().gp0(v);
    return;
  } // GP0 (direct)
  if (p == 0x1F801814) {
    psx::gpu::gp1Write(*this, v);
    return;
  } // GP1 (display/control)
  if (p == 0x1F801820 || p == 0x1F801824) { // MDEC0 cmd / MDEC1 ctrl
    game->mdec.write(p, v);
    // A command or control write can unblock a pending transfer.
    if (dma.mdec0Left > 0 || dma.mdec1Left > 0) {
      mdec_dma_pump();
    }
    return;
  }
  if (p == 0x1F801DA6) {
    dma.spuXferAddr = (v & 0xFFFF) << 3; // SPU transfer-start addr (bytes)
  }
  if (p >= 0x1F801C00 && p <= 0x1F801FFF) {
    psx::audio::spu::write(p, v);
    return;
  } // SPU register file
  // DMA3: CD sector data to RAM, as stock libcd CD_getsector drives it.
  if (p == 0x1F8010B0) {
    dma.of(DmaChannel::Cdrom).madr = v;
    return;
  }
  if (p == 0x1F8010B4) {
    dma.of(DmaChannel::Cdrom).bcr = v;
    return;
  }
  if (p == 0x1F8010B8) {
    dma.of(DmaChannel::Cdrom).chcr = v;
    if (v & 0x01000000u) { // start/busy
      // Mode 2 ignores BCR and walks the chain at MADR.
      const unsigned mode = psx::dma::syncMode(v);
      uint32_t endMadr = dma.of(DmaChannel::Cdrom).madr & 0x1FFFFC;
      bool chainRefused = false;
      int n = mode == psx::dma::kLinkedList ? psx::dma::chainWords(*this, dma.of(DmaChannel::Cdrom).madr, &endMadr, &chainRefused)
                                            : dma_block_words(dma.of(DmaChannel::Cdrom).bcr);
      if (mode != psx::dma::kLinkedList) {
        endMadr = (dma.of(DmaChannel::Cdrom).madr & 0x1FFFFC) + (unsigned)n * 4u;
      }
      if (n > 0x10000) {
        n = 0x10000;
      }
      uint32_t da = dma.of(DmaChannel::Cdrom).madr & 0x1FFFFC;
      // CDC DMA reads zero past the FIFO and still completes; both counts are logged.
      if (mode == psx::dma::kLinkedList) {
        // Each chain node's payload takes the next FIFO words in order.
        int remaining = n;
        uint32_t node = da;
        for (int hops = 0; hops < psx::dma::kChainWordCap && remaining > 0; hops++) {
          const uint32_t header = mem_r32(node);
          const int count = (int)(header >> 24);
          const uint32_t next = header & 0x00FFFFFFu;
          const int take = count < remaining ? count : remaining;
          const int got = game->cdc.dmaRead(dmaBuffer_.data() + (n - remaining), take);
          for (int i = 0; i < take; i++) {
            mem_w32_unnotified(node + 4 + (unsigned)i * 4u, dmaBuffer_[n - remaining + i]);
          }
          psx::cpu::notifyExecutableWrite(
              *this, {node + 4, node + 4 + (unsigned)take * 4u}, psx::cpu::ExecutableWriteSource::Dma);
          psx::code_module::publishLanding(*this, {node + 4, node + 4 + (unsigned)take * 4u});
          lucent::debug("cdc",
                        "DMA3 chain node 0x{:08X}: {} words, FIFO {} + controller-zero {} (head LBA {})",
                        0x80000000u | node,
                        take,
                        got,
                        take - got,
                        game->cdc.loc_lba);
          remaining -= take;
          if (next == 0x00FFFFFFu || remaining <= 0) {
            break;
          }
          node = next & 0x1FFFFC;
        }
      } else {
        const int fifo_words = game->cdc.dmaRead(dmaBuffer_.data(), n);
        for (int i = 0; i < n; i++) {
          mem_w32_unnotified(da + (unsigned)i * 4u, dmaBuffer_[i]);
        }
        psx::cpu::notifyExecutableWrite(*this, {da, da + (unsigned)n * 4u}, psx::cpu::ExecutableWriteSource::Dma);
        psx::code_module::publishLanding(*this, {da, da + (unsigned)n * 4u});
        lucent::debug("cdc",
                      "DMA3 {} words -> 0x{:08X}: FIFO {} + controller-zero {} (head LBA {})",
                      n,
                      0x80000000u | da,
                      fifo_words,
                      n - fifo_words,
                      game->cdc.loc_lba);
      }
      if (mode == psx::dma::kLinkedList && !chainRefused) {
        // Hardware leaves MADR at the chain's end.
        dma.of(DmaChannel::Cdrom).madr = endMadr;
        lucent::debug("cdc", "DMA3 chain {} words, MADR advanced to 0x{:08X}", n, 0x80000000u | endMadr);
      }
      dma.of(DmaChannel::Cdrom).chcr &= ~0x01000000u; // clear busy: the completion poll must pass
      irqStatLatch();               // draining a sector queues the next INT1
      // Completion is announced only for a channel the guest armed in DICR (libstr arms the last sector only).
      if (dma_completed(dma, DmaChannel::Cdrom)) {
        pending_work |= PW_IRQ;
      }
    }
    return;
  }
  if (p == 0x1F8010C0) {
    dma.of(DmaChannel::Spu).madr = v;
    return;
  }
  if (p == 0x1F8010C4) {
    dma.of(DmaChannel::Spu).bcr = v;
    return;
  }
  if (p == 0x1F8010C8) { // DMA4 CHCR: SPU-RAM transfer
    dma.of(DmaChannel::Spu).chcr = v;
    if (v & 0x01000000u) {
      int n = dma_block_words(dma.of(DmaChannel::Spu).bcr);
      if (n > 0x10000) {
        n = 0x10000;
      }
      uint32_t da = dma.of(DmaChannel::Spu).madr & 0x1FFFFC;
      if (v & 1) { // RAM -> SPU
        for (int i = 0; i < n; i++) {
          dmaBuffer_[i] = mem_r32(da + i * 4);
        }
        lucent::debug(spuDmaChannel_,
                      "RAM 0x{:08X} -> SPU 0x{:06X}  {} words ({} B)  pc={:08X}",
                      psx::kseg0Address(da),
                      dma.spuXferAddr,
                      n,
                      n * 4,
                      pc);
        // A large VAB bank: name the engine-range return addresses on the stack.
        if (n > 20000 && spuDmaChannel_) {
          uint32_t sp = psx::physical(r[psx::cpu::kSp]);
          lucent::Line ln;
          ln.add("  [vab-caller-chain]");
          for (uint32_t o = 0; o < 0x800 && sp + o + 4 <= psx::kMainRamBytes; o += 4) {
            uint32_t w;
            memcpy(&w, &ram[sp + o], 4);
            uint32_t p = psx::physical(w);
            if ((w & 0xFFE00000u) == 0x80000000u && (w & 3) == 0 && p >= 0x1E000 && p < 0x82000) {
              ln.add(" {:08X}", w);
            }
          }
          ln.flush(lucent::Level::Info, "spudma");
        }
        psx::audio::spu::dmaWrite(dmaBuffer_.data(), n);
      } else { // SPU -> RAM
        int got = psx::audio::spu::dmaRead(dmaBuffer_.data(), n);
        for (int i = 0; i < got; i++) {
          mem_w32_unnotified(da + i * 4, dmaBuffer_[i]);
        }
        psx::cpu::notifyExecutableWrite(*this, {da, da + (unsigned)got * 4u}, psx::cpu::ExecutableWriteSource::Dma);
      }
      dma.of(DmaChannel::Spu).chcr &= ~0x01000000u;
      if (dma_completed(dma, DmaChannel::Spu)) {
        pending_work |= PW_IRQ;
      }
      // The synchronous transfer is done, so its HwSPU event (class 0xF0000009) is due now.
      game->hle.deliverEvent(0xF0000009u, 0xFFFFFFFFu);
    }
    return;
  }
  if (p == 0x1F801080) {
    dma.of(DmaChannel::MdecIn).madr = v;
    return;
  }
  if (p == 0x1F801084) {
    dma.of(DmaChannel::MdecIn).bcr = v;
    return;
  }
  if (p == 0x1F801088) { // DMA0 CHCR: MDEC-in (RAM -> MDEC)
    dma.of(DmaChannel::MdecIn).chcr = v;
    if (v & 0x01000000u) {
      lucent::debug("mdecdma",
                    "DMA0(in)  start madr={:08X} bcr={:08X} words={}",
                    dma.of(DmaChannel::MdecIn).madr,
                    dma.of(DmaChannel::MdecIn).bcr,
                    dma_block_words(dma.of(DmaChannel::MdecIn).bcr));
      int n = dma_block_words(dma.of(DmaChannel::MdecIn).bcr);
      if (n > 0x10000) {
        n = 0x10000;
      }
      dma.mdec0Addr = dma.of(DmaChannel::MdecIn).madr & 0xFFFFFCu;
      dma.mdec0Left = n;
      mdecStallReported_ = false;
      mdecDeferNote_ = 0;
      // Busy stays set until the pump feeds the last word.
      mdec_dma_pump();
    } else if (dma.mdec0Left > 0) {
      // Clearing bit 24 aborts the channel (dma.c DMA_Write).
      lucent::debug("mdecdma", "DMA0(in)  forced stop: {} word(s) abandoned", dma.mdec0Left);
      dma.mdec0Left = 0;
    }
    return;
  }
  if (p == 0x1F801090) {
    dma.of(DmaChannel::MdecOut).madr = v;
    return;
  }
  if (p == 0x1F801094) {
    dma.of(DmaChannel::MdecOut).bcr = v;
    return;
  }
  if (p == 0x1F801098) { // DMA1 CHCR: MDEC-out (MDEC -> RAM)
    dma.of(DmaChannel::MdecOut).chcr = v;
    if (v & 0x01000000u) {
      lucent::debug("mdecdma",
                    "DMA1(out) start madr={:08X} bcr={:08X} words={} canread={}",
                    dma.of(DmaChannel::MdecOut).madr,
                    dma.of(DmaChannel::MdecOut).bcr,
                    dma_block_words(dma.of(DmaChannel::MdecOut).bcr),
                    (int)game->mdec.canRead());
      int n = dma_block_words(dma.of(DmaChannel::MdecOut).bcr);
      if (n > 0x10000) {
        n = 0x10000;
      }
      dma.mdec1Addr = dma.of(DmaChannel::MdecOut).madr & 0xFFFFFFu; // dma.c CurAddr form: advances 4/word, 0x1FFFFC at store
      dma.mdec1Left = n;
      mdecStallReported_ = false;
      mdecDeferNote_ = 0;
      mdec_dma_pump();
    } else if (dma.mdec1Left > 0) {
      lucent::debug("mdecdma", "DMA1(out) forced stop: {} word(s) abandoned", dma.mdec1Left);
      dma.mdec1Left = 0;
    }
    return;
  }
  if (p == 0x1F8010A0) {
    dma.of(DmaChannel::Gpu).madr = v;
    return;
  }
  if (p == 0x1F8010A4) {
    dma.of(DmaChannel::Gpu).bcr = v;
    return;
  }
  if (p == 0x1F8010A8) { // DMA2 CHCR: start triggers the transfer
    dma.of(DmaChannel::Gpu).chcr = v;
    if (v & 0x01000000u) { // start/busy
      const int sync = static_cast<int>((v >> 9) & 3);
      const int to_gpu = static_cast<int>(v & 1);
      if (sync == 2) {
        psx::gpu::dma2LinkedList(*this, dma.of(DmaChannel::Gpu).madr); // ordering-table linked list — full walk
      } else if (sync == 1) {
        // block: BC = blocks*size words
        psx::gpu::dma2Block(*this, {dma.of(DmaChannel::Gpu).madr, (dma.of(DmaChannel::Gpu).bcr & 0xFFFF) * (dma.of(DmaChannel::Gpu).bcr >> 16) * 4u}, to_gpu != 0);
      } else {
        psx::gpu::dma2Block(*this, {dma.of(DmaChannel::Gpu).madr, (dma.of(DmaChannel::Gpu).bcr & 0xFFFF) * 4u}, to_gpu != 0); // immediate
      }
      dma.of(DmaChannel::Gpu).chcr &= ~0x01000000u; // clear busy -> game's DMA-done poll passes
      if (dma_completed(dma, DmaChannel::Gpu)) {
        pending_work |= PW_IRQ;
      }
    }
    return;
  }
  if (p >= 0x1F8010F0 && p <= 0x1F8010F3) { // DPCR
    const GuestStore store{p, v, bytes};
    const uint32_t lane = psx::dma::laneMask(store);
    dma.dpcr = (dma.dpcr & ~lane) | (psx::dma::laneValue(store) & lane);
    return;
  }
  if (p >= 0x1F8010F4 && p <= 0x1F8010F7) { // DICR
    dma.dicr = psx::dma::storeDicr(dma.dicr, {p, v, bytes});
    unsigned armed = 0;
    for (uint32_t ch = 0; ch < psx::dma::kChannelCount; ch++) {
      if (psx::dma::irqArmed(dma.dicr, static_cast<DmaChannel>(ch))) {
        armed |= 1u << ch;
      }
    }
    lucent::debug("dmairq",
                  "w DICR{}[{}] = {:08X} -> {:08X} (armed channel mask {:02X}) ra={:08X}",
                  (p & 3u),
                  bytes,
                  v,
                  psx::dma::readDicr(dma.dicr),
                  armed,
                  r[psx::cpu::kRa]);
    return;
  }
  if (p == 0x1F8010E0) {
    dma.of(DmaChannel::Otc).madr = v;
    return;
  }
  if (p == 0x1F8010E4) {
    dma.of(DmaChannel::Otc).bcr = v;
    return;
  }
  if (p == 0x1F8010E8) { // DMA6 CHCR: OTC (ordering-table clear)
    dma.of(DmaChannel::Otc).chcr = v;
    if (v & 0x01000000u) { // start/busy
      uint32_t n = dma.of(DmaChannel::Otc).bcr & 0xFFFF;
      if (n == 0) {
        n = 0x10000;
      }
      uint32_t madr = dma.of(DmaChannel::Otc).madr & 0x1FFFFC; // highest entry address (&ot[n-1])
      for (uint32_t i = 0; i < n; i++) {
        uint32_t addr = madr - i * 4;
        uint32_t word = (i == n - 1) ? 0x00FFFFFFu : ((addr - 4) & 0x00FFFFFFu);
        mem_w32_unnotified(addr, word);
      }
      psx::cpu::notifyExecutableWrite(*this, {madr - (n - 1) * 4u, madr + 4u}, psx::cpu::ExecutableWriteSource::Dma);
      dma.of(DmaChannel::Otc).chcr &= ~0x01000000u; // clear busy -> ClearOTagR's busy-poll passes
      if (dma_completed(dma, DmaChannel::Otc)) {
        pending_work |= PW_IRQ;
      }
    }
    return;
  }
  warn_unmapped_ram(this, a, bytes, "write");
  lucent::debug("io", "write{} @ 0x{:08X} = 0x{:08X}", bytes * 8, a, v); // discarded: no such register
}

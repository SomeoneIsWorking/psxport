// The guest SPU interface over Beetle's spu.c (compiled as-is), and the externs spu.c links against.
#include "spu_beetle.h"
#include "core.h"
#include "diagnostic_config.h"
#include "game.h"
#include "host_backtrace.h"
#include "spu_device.h"
#include "spu_field_cadence.h"
#include "spu_state.h"
#include <lucent/log.h>
#include <stdbool.h>
#include <stdint.h>

#include <algorithm>

namespace {

constexpr lucent::Channel spuChannel{"spu"};

constexpr uint32_t kKeyOnLow = 0x188;
constexpr uint32_t kKeyOffHigh = 0x18E;
constexpr std::size_t kKeyOffHalf = 2;

// Run `clocks`; a key latch written before a sample that ran has been taken.
void run(SpuDevice &spu, int32_t clocks) {
  if (clocks <= 0) {
    return;
  }
  const bool sampled = spu.untilSample <= 0 || clocks >= spu.untilSample;
  spu.untilSample = SPU_UpdateFromCDC(clocks);
  spu.fieldRun += clocks;
  if (sampled) {
    spu.pendingKeys = {};
  }
}

// Run the SPU to the guest's emulated time. A guest past its field's end (frame() comes at the title's own
// boundary) runs on into the next field, at most one field ahead.
void catchUp(SpuDevice &spu) {
  if (!spu.fieldOpen || spu.core() == nullptr || spu.core()->game == nullptr) {
    return;
  }
  const uint64_t now = spu.core()->game->timing.emulatedCpuTicks();
  const uint64_t limit = 2 * static_cast<uint64_t>(spu.fieldBudget);
  const uint64_t elapsed = now > spu.fieldOrigin ? std::min(now - spu.fieldOrigin, limit) : 0;
  run(spu, static_cast<int32_t>(elapsed) - spu.fieldRun);
}

// Writes with no guest time between them (a native override, a guest a field ahead) still reach a
// key latch before the SPU samples it; run to the next sample so the latch is taken before it is rewritten.
void takePendingKeys(SpuDevice &spu, uint32_t off, uint16_t val) {
  if (off < kKeyOnLow || off > kKeyOffHigh || (off & 1) != 0) {
    return;
  }
  const std::size_t half = (off - kKeyOnLow) / 2;
  const bool offAfterOn = half >= kKeyOffHalf && (spu.pendingKeys[half - kKeyOffHalf] & val) != 0;
  if (spu.pendingKeys[half] != 0 || offAfterOn) {
    run(spu, spu.untilSample > 0 ? spu.untilSample : static_cast<int32_t>(SpuFieldCadence::kClocksPerSample));
  }
  spu.pendingKeys[half] |= val;
}

} // namespace

SpuDevice::SpuDevice() : writeBacktraceOffset(psx::config::spu_write_backtrace()) {}

extern "C" {
#include "mednafen/psx/irq.h"

// SPU update granularity in 44.1 kHz samples per inner step; 1 is upstream's full accuracy.
uint8_t spu_samples = 1;

// Skip fully silent voices; spu.c documents the output as bit-identical.
bool psx_spu_silent_voice_opt = true;

// spu.c raises IRQ_SPU on an SPU-RAM address match; gpu.c's VBlank and GP0(1Fh) lines are owned elsewhere.
void IRQ_Assert(int which, bool asserted) {
  const SpuDevice *const spu = SpuDevice::bound();
  if (which != IRQ_SPU || spu == nullptr || spu->core() == nullptr) {
    return;
  }
  psx::audio::spu::raiseIrq(*spu->core(), asserted);
}

} // extern "C"

// Beetle addresses the registers at 0x1F801C00.. by their low 10 bits; timestamp 0 is its untimed access,
// so every access first runs the SPU to the guest's time (catchUp).
namespace psx::audio::spu {

void init() {
  SPU_Init();
  SPU_Power();
}

void write(uint32_t addr, uint32_t val) {
  const uint32_t off = addr & 0x3FF;
  SpuDevice &spu = *SpuDevice::bound();
  catchUp(spu);
  if (spuChannel) {
    DebugCounters &sd = spu.debug;
    const long n = ++sd.writes;
    if (off < 0x180) {
      sd.voiceWrites++;
    } else if (off == 0x18C || off == 0x18E) {
      sd.keyOffWrites++;
    } else if (off == 0x1B0 || off == 0x1B2) {
      sd.cdVolumeWrites++;
    } else if (off == 0x180 || off == 0x182) {
      sd.mainVolumeWrites++;
    }
    if (off < 0x180) {
      const uint32_t vreg = off & 0xF;
      const uint32_t vno = off >> 4;
      if (vreg == 0x4) {
        lucent::debug(spuChannel, "[spudbg] V{:02} pitch={:04X}", vno, val & 0xFFFF);
      } else if (vreg == 0x6) {
        lucent::debug(spuChannel, "[spudbg] V{:02} start={:04X}", vno, val & 0xFFFF);
      } else if (vreg == 0x8 || vreg == 0xA) {
        lucent::debug(spuChannel, "[spudbg] V{:02} adsr{}={:04X}", vno, vreg == 0x8 ? 1 : 2, val & 0xFFFF);
      }
    }
    if (off == 0x188 || off == 0x18A) {
      lucent::debug(
          spuChannel, "[spudbg] KON off={:03X} val={:04X} (writes={}) field+{}", off, val & 0xFFFF, n, spu.fieldRun);
    } else if (off == 0x18C || off == 0x18E) {
      lucent::debug(
          spuChannel, "[spudbg] KOFF off={:03X} val={:04X} (writes={}) field+{}", off, val & 0xFFFF, n, spu.fieldRun);
    } else if (off == 0x1AA) {
      lucent::debug(spuChannel,
                    "[spudbg] SPUCNT={:04X} enable={} cdaudio={} reverb={} xfermode={} (writes={})",
                    val & 0xFFFF,
                    (val >> 15) & 1,
                    val & 1,
                    (val >> 7) & 1,
                    (val >> 4) & 3,
                    n);
    } else if (off == 0x184 || off == 0x186 || off == 0x198 || off == 0x19A || off == 0x1A2 ||
               (off >= 0x1C0 && off < 0x200)) {
      // Reverb output volume, per-voice reverb enable, work-area base and the 0x1C0.. configuration.
      lucent::debug(spuChannel, "[spudbg] REVERB off={:03X} val={:04X} (writes={})", off, val & 0xFFFF, n);
    } else if (off == 0x1B0 || off == 0x1B2) {
      lucent::debug(spuChannel, "[spudbg] CDVOL off={:03X} val={:04X} (writes={})", off, val & 0xFFFF, n);
    } else if (off == 0x1A6) {
      sd.transferAddress = (val & 0xFFFF) << 3;
      lucent::debug(spuChannel, "[spudbg] SPU xfer ADDR=0x{:05X}", sd.transferAddress);
    } else if (off == 0x1A8) {
      sd.dataPortWrites++;
      if ((sd.dataPortWrites % 1000) == 1) {
        lucent::debug(spuChannel, "[spudbg] SPU DATA-port write #{} (val={:04X})", sd.dataPortWrites, val & 0xFFFF);
      }
    }
    if ((n % 20000) == 0) {
      lucent::debug(spuChannel,
                    "[spudbg] SUMMARY writes={} voice={} koff={} mainvol={} cdvol={}",
                    n,
                    sd.voiceWrites,
                    sd.keyOffWrites,
                    sd.mainVolumeWrites,
                    sd.cdVolumeWrites);
    }
  }
  // MMIO is invisible to guest write watches; the host stack names the native chain that wrote.
  if (spu.writeBacktraceOffset == off) {
    void *frames[32];
    const int n = backtrace(frames, 32);
    lucent::info("spubt", "off={:03X} val={:04X}", off, val & 0xFFFF);
    psxport::host::emitBacktrace(
        lucent::Level::Info, "spubt", std::span<void *const>(frames, static_cast<std::size_t>(n)).subspan(1));
    lucent::info("spubt", "----");
  }
  takePendingKeys(spu, off, static_cast<uint16_t>(val));
  SPU_Write(0, off, static_cast<uint16_t>(val));
}

uint32_t read(uint32_t addr) {
  catchUp(*SpuDevice::bound());
  return SPU_Read(0, addr & 0x3FF);
}

// DMA4: one 32-bit word (two SPU-RAM samples) per Beetle call.
void dmaWrite(const uint32_t *words, int count) {
  catchUp(*SpuDevice::bound());
  if (spuChannel) {
    DebugCounters &sd = SpuDevice::bound()->debug;
    sd.dmaCalls++;
    sd.dmaWords += count;
    lucent::debug(
        spuChannel, "[spudbg] SPU-RAM DMA write: {} words (call {}, total {} words)", count, sd.dmaCalls, sd.dmaWords);
  }
  for (int i = 0; i < count; i++) {
    SPU_WriteDMA(words[i]);
  }
}

int dmaRead(uint32_t *words, int count) {
  catchUp(*SpuDevice::bound());
  for (int i = 0; i < count; i++) {
    words[i] = SPU_ReadDMA();
  }
  return count;
}

// Fields follow each other on the emulated clock; a clock more than a field away (a restored state, a guest
// call that ran ahead) starts the field where the clock is.
void beginField(uint64_t now, int32_t clocks) {
  SpuDevice &spu = *SpuDevice::bound();
  spu.fieldOrigin = spu.fieldStarted ? spu.fieldOrigin + static_cast<uint64_t>(spu.fieldBudget) : now;
  const int64_t offset = static_cast<int64_t>(now - spu.fieldOrigin);
  if (offset < -clocks || offset > clocks) {
    spu.fieldOrigin = now;
  }
  spu.fieldStarted = true;
  spu.fieldOpen = true;
  spu.fieldBudget = clocks;
}

void finishField(int32_t clocks) {
  SpuDevice &spu = *SpuDevice::bound();
  run(spu, clocks - spu.fieldRun);
  spu.fieldRun = std::max(spu.fieldRun - clocks, 0);
  spu.fieldOpen = false;
}

int render(int16_t *out, int max_frames) {
  return SPU_Render(out, max_frames);
}

} // namespace psx::audio::spu

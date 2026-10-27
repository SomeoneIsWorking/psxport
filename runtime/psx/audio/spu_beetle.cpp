// The guest SPU interface over Beetle's spu.c (compiled as-is), and the externs spu.c links against.
#include "spu_beetle.h"
#include "diagnostic_config.h"
#include "host_backtrace.h"
#include "spu_device.h"
#include "spu_field_cadence.h"
#include "spu_state.h"
#include <lucent/log.h>
#include <stdbool.h>
#include <stdint.h>

namespace {

constexpr lucent::Channel spuChannel{"spu"};

constexpr uint32_t kKeyOnLow = 0x188;
constexpr uint32_t kKeyOffHigh = 0x18E;
constexpr std::size_t kKeyOffHalf = 2;

// Untimed writes land before the field's samples; run one so a key latch is taken before it is rewritten.
void takePendingKeys(SpuDevice &spu, uint32_t off, uint16_t val) {
  if (off < kKeyOnLow || off > kKeyOffHigh || (off & 1) != 0) {
    return;
  }
  const std::size_t half = (off - kKeyOnLow) / 2;
  const bool offAfterOn = half >= kKeyOffHalf && (spu.pendingKeys[half - kKeyOffHalf] & val) != 0;
  if (spu.pendingKeys[half] != 0 || offAfterOn) {
    SPU_UpdateFromCDC(static_cast<int32_t>(SpuFieldCadence::kClocksPerSample));
    spu.clocksAhead += static_cast<int32_t>(SpuFieldCadence::kClocksPerSample);
    spu.pendingKeys = {};
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

// Beetle addresses the registers at 0x1F801C00.. by their low 10 bits; timestamp 0 is its untimed access.
namespace psx::audio::spu {

void init() {
  SPU_Init();
  SPU_Power();
}

void write(uint32_t addr, uint32_t val) {
  const uint32_t off = addr & 0x3FF;
  SpuDevice &spu = *SpuDevice::bound();
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
      lucent::debug(spuChannel, "[spudbg] KON off={:03X} val={:04X} (writes={})", off, val & 0xFFFF, n);
    } else if (off == 0x18C || off == 0x18E) {
      lucent::debug(spuChannel, "[spudbg] KOFF off={:03X} val={:04X} (writes={})", off, val & 0xFFFF, n);
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
  return SPU_Read(0, addr & 0x3FF);
}

// DMA4: one 32-bit word (two SPU-RAM samples) per Beetle call.
void dmaWrite(const uint32_t *words, int count) {
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
  for (int i = 0; i < count; i++) {
    words[i] = SPU_ReadDMA();
  }
  return count;
}

int32_t update(int32_t clocks) {
  SpuDevice &spu = *SpuDevice::bound();
  const int32_t owed = clocks - spu.clocksAhead;
  spu.clocksAhead = owed < 0 ? -owed : 0;
  if (owed <= 0) {
    return 0;
  }
  spu.pendingKeys = {};
  return SPU_UpdateFromCDC(owed);
}

int render(int16_t *out, int max_frames) {
  return SPU_Render(out, max_frames);
}

} // namespace psx::audio::spu

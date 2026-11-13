// spu_device.cpp — the SPU's interrupt line into the bound Core's I_STAT.
#include "spu_device.h"
#include "blob_codec.h"
#include "core.h"
#include "game.h"
#include "irq_edge.h"

void psx::audio::spu::raiseIrq(Core &core, bool asserted) {
  if (psx::platform::latchIrqEdge(core.game->spu.irqLevel, IRQ_BIT_SPU, asserted) != 0u) {
    core.raiseInterrupt(IRQ_BIT_SPU);
  }
}

void SpuDevice::saveField(psx::platform::BlobWriter &out) const {
  for (const std::uint16_t keys : pendingKeys) {
    out.u16(keys);
  }
  out.boolean(fieldOpen);
  out.boolean(fieldStarted);
  out.u64(fieldOrigin);
  out.i32(fieldBudget);
  out.i32(fieldRun);
  out.i32(untilSample);
}

bool SpuDevice::loadField(psx::platform::BlobReader &in, std::string &error) {
  for (std::uint16_t &keys : pendingKeys) {
    keys = in.u16();
  }
  fieldOpen = in.boolean();
  fieldStarted = in.boolean();
  fieldOrigin = in.u64();
  fieldBudget = in.i32();
  fieldRun = in.i32();
  untilSample = in.i32();
  if (!in.ok() || fieldBudget < 0 || fieldRun < 0) {
    error = "beetle.spu section: the SPU's field run does not decode";
    return false;
  }
  return true;
}

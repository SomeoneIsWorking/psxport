#include "beetle_device_state.h"

#include "core.h"
#include "game.h"

#include <cstdlib>

using psx::state::BeetleStateMem;

extern "C" {
int SPU_StateAction(BeetleStateMem *sm, int load, int data_only);
int MDEC_StateAction(BeetleStateMem *sm, int load, int data_only);

// state.c's MDFNSS_SaveSM references this symbol.
int StateAction(BeetleStateMem *sm, int load, int data_only) {
  int ret = 1;
  ret &= SPU_StateAction(sm, load, data_only);
  ret &= MDEC_StateAction(sm, load, data_only);
  return ret;
}
} // extern "C"

namespace psx::state {
namespace {

int (*stateActionFor(BeetleDevice device))(BeetleStateMem *, int, int) {
  return device == BeetleDevice::Spu ? &SPU_StateAction : &MDEC_StateAction;
}

const char *deviceName(BeetleDevice device) {
  return device == BeetleDevice::Spu ? "SPU" : "MDEC";
}

// Beetle's bindings are process-global; this machine's instance stays bound, as after a frame step.
// An unpowered device is not powered here, which would invent state.
void bindDevice(Core &core, BeetleDevice device) {
  if (device == BeetleDevice::Spu) {
    core.game->spu.bindExisting(&core);
    return;
  }
  core.game->mdec.bindExisting();
}

} // namespace

BeetleDeviceState::~BeetleDeviceState() {
  clear();
}

void BeetleDeviceState::clear() {
  std::free(sm.data);
  sm = BeetleStateMem{};
}

std::vector<std::uint8_t> BeetleDeviceState::bytesOut() const {
  return std::vector<std::uint8_t>(sm.data, sm.data + sm.len);
}

bool BeetleDeviceState::save(Core &core, BeetleDevice device, std::string &error) {
  error.clear();
  bindDevice(core, device);
  clear();
  const int ok = stateActionFor(device)(&sm, 0, 0);
  if (ok == 0) {
    error = std::string("Beetle ") + deviceName(device) + "_StateAction refused to save";
    return false;
  }
  if (sm.len == 0) {
    error = std::string("Beetle ") + deviceName(device) + "_StateAction wrote 0 bytes";
    return false;
  }
  return true;
}

bool BeetleDeviceState::load(Core &core,
                             BeetleDevice device,
                             std::span<const std::uint8_t> payload,
                             std::string &error) {
  error.clear();
  if (payload.empty()) {
    error = std::string("Beetle ") + deviceName(device) + " section is empty";
    return false;
  }
  bindDevice(core, device);
  // Non-owning: the fork's load path never reaches the realloc in smem_write.
  BeetleStateMem source{};
  source.data = const_cast<std::uint8_t *>(payload.data());
  source.len = static_cast<std::uint32_t>(payload.size());
  const int ok = stateActionFor(device)(&source, 1, 0);
  if (ok == 0) {
    error = std::string("Beetle ") + deviceName(device) + "_StateAction could not read its section (" +
            std::to_string(payload.size()) + " bytes offered)";
    return false;
  }
  return true;
}

} // namespace psx::state
#include "beetle_device_state.h"

#include "core.h"
#include "game.h"
#include "mdec_device.h"
#include "spu_device.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

// Beetle state framework (mednafen/state.h) and the two lifted device state actions. Declared
// locally, exactly as every other adapter in this tree declares a Beetle API, so this module does
// not pull the vendor headers (which drag in mednafen-types.h and the retro shims).
// `StateMem` is a plain struct with C linkage-compatible layout; only the FUNCTION names are C.
using psx::state::BeetleStateMem;

// Guest-facing device register writes (mem.cpp / spu_beetle.cpp), C linkage.
extern "C" void spu_write(uint32_t addr, uint32_t val);
extern "C" void mdec_write(uint32_t addr, uint32_t val);

extern "C" {
int MDFNSS_SaveSM(void *st, int a, int b, const void *c, const void *d, const void *e);
int MDFNSS_LoadSM(void *st, int a, int b);
int SPU_StateAction(BeetleStateMem *sm, int load, int data_only);
int MDEC_StateAction(BeetleStateMem *sm, int load, int data_only);

// The whole-machine entry point every Mednafen system driver defines, and the one this port does
// NOT use to save a machine: psx::state::MachineState owns the machine and composes its devices
// itself, one named section each.
//
// It exists because the fork's MDFNSS_SaveSM / MDFNSS_LoadSM (its whole-stream header plus chunk
// driver) call it, and because beetleDeviceRoundTrips drives exactly those two functions — the only
// thing in this tree that proves the fork's chunk WRITER and chunk READER agree with each other,
// rather than each agreeing with itself in one direction.
//
// The GTE is deliberately absent: psxport's GTE state is `GteRegs` (the 64 register words and
// FLAGS), which the whole-machine owner writes directly, and Beetle's GTE_StateAction adds
// Matrices/CRVectors duplicates kept only for compatibility with savestates this port does not read.
int StateAction(BeetleStateMem *sm, int load, int data_only);
} // extern "C"

int StateAction(BeetleStateMem *sm, int load, int data_only) {
  int ret = 1;
  ret &= SPU_StateAction(sm, load, data_only);
  ret &= MDEC_StateAction(sm, load, data_only);
  return ret;
}

namespace psx::state {
namespace {

int (*stateActionFor(BeetleDevice device))(BeetleStateMem *, int, int) {
  return device == BeetleDevice::Spu ? &SPU_StateAction : &MDEC_StateAction;
}

const char *deviceName(BeetleDevice device) {
  return device == BeetleDevice::Spu ? "SPU" : "MDEC";
}

// Bind this machine's own instance for the duration of one state call. Beetle's SPU/MDEC bindings
// are process-global with no getter in the fork, so the previous binding cannot be read back and
// restored; leaving this machine's instance bound is the correct end state anyway, because it IS
// this machine's instance — exactly what SpuDevice::bind / MdecDevice::bind already leave bound
// after a frame step.
//
// `powerIfNeeded` is false for the state path on purpose. A device that was never powered on has
// no state to save, and powering it here would fabricate a machine state that did not exist.
void bindDevice(Core &core, BeetleDevice device, bool powerIfNeeded) {
  if (device == BeetleDevice::Spu) {
    if (powerIfNeeded) {
      core.game->spu.bind(&core);
    } else {
      core.game->spu.bindExisting(&core);
    }
    return;
  }
  if (powerIfNeeded) {
    core.game->mdec.bind();
  } else {
    core.game->mdec.bindExisting();
  }
}

// Move the bound device away from power-on, with the register values derived from `seed`.
//
// Every address written here is one a running title writes, and every one of them is inside the
// fork's own SFVARN / SFARRAY16 list, so the bytes that move are bytes the state format carries.
// This is the selftest's fixture AND its perturbation in one: there is no reset call here because
// SpuDevice::bind / MdecDevice::bind power on only ONCE per instance, so a "return it to power-on"
// step would silently do nothing on the second call and the round-trip would compare a device
// against itself.
void perturbDevice(Core &core, BeetleDevice device, std::uint32_t seed) {
  bindDevice(core, device, true);
  if (device == BeetleDevice::Spu) {
    spu_write(0x1F801DA6u, seed & 0xFFFFu);        // transfer address
    spu_write(0x1F801DAAu, 0x1234u ^ seed);        // IRQ address
    spu_write(0x1F801DA8u, 0x0020u | (seed & 1u)); // control: SPU IRQ enable
    spu_write(0x1F801D80u, 0x7F7Fu ^ seed);        // main volume L/R
    spu_write(0x1F801DB0u, 0x0F0Fu ^ seed);        // CD volume L/R
    spu_write(0x1F801D00u, seed & 0x7FFFu);        // voice 0 left volume
    spu_write(0x1F801D02u, (seed >> 4) & 0x7FFFu); // voice 0 right volume
    spu_write(0x1F801D08u, 0x0001u);               // voice 0 key on
    return;
  }
  mdec_write(0x1F801E10u, 0x00040004u | (seed & 0xFFu)); // command 0x1 + a parameter word
  mdec_write(0x1F801E10u, 0x30000000u | (seed & 0xFFFFu));
  mdec_write(0x1F801E10u, 0x38000000u);
}

} // namespace

BeetleDeviceState::~BeetleDeviceState() {
  clear();
}

void BeetleDeviceState::clear() {
  std::free(sm.data);
  sm = BeetleStateMem{};
}

BeetleStateMem &BeetleDeviceState::rawStream() {
  return sm;
}

std::vector<std::uint8_t> BeetleDeviceState::bytesOut() const {
  return std::vector<std::uint8_t>(sm.data, sm.data + sm.len);
}

bool BeetleDeviceState::save(Core &core, BeetleDevice device, std::string &error) {
  error.clear();
  bindDevice(core, device, false);
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
  bindDevice(core, device, false);
  // A NON-owning stream over the section's bytes. The fork's load path only reads through it
  // (`smem_read` / `smem_seek`); `smem_write` — the only function that would `realloc` `data` — is
  // never reached with `load` set, so `malloced`/`initial_malloc` are never consulted.
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

bool beetleDeviceRoundTrips(Core &core, BeetleDevice device, std::string &error) {
  error.clear();
  const char *name = deviceName(device);

  // 1. The state to be restored, taken with the device moved off power-on so there is something to
  //    distinguish. Without that, every comparison below is a device against itself.
  perturbDevice(core, device, 0x00A5u);
  BeetleDeviceState captured;
  if (!captured.save(core, device, error)) {
    return false;
  }
  const std::vector<std::uint8_t> before = captured.bytesOut();

  // 2. The fork's WHOLE-STREAM writer, `MDFNSS_SaveSM`: a 32-byte header plus one chunk per device,
  //    reached through the `StateAction` aggregate above. Measured here, and only here, because psxport
  //    does NOT save a machine through it — see beetle_device_state.h for what was measured and why
  //    the per-chunk path is the one production uses. Two consecutive captures of an unchanged device
  //    are NOT byte-identical here (the first difference falls well inside the SPU chunk, long before
  //    the 512 KB SPURAM tail), so the whole-stream bytes are not even a stable expectation to compare
  //    a restored device against; what IS asserted is the shape it writes and that the fork's own
  //    reader accepts it.
  BeetleDeviceState whole;
  if (MDFNSS_SaveSM(&whole.rawStream(), 0, 0, nullptr, nullptr, nullptr) == 0) {
    error = std::string("Beetle MDFNSS_SaveSM refused to write a stream");
    return false;
  }
  {
    const BeetleStateMem &stream = whole.rawStream();
    const bool headerOk = stream.len > 32 + 36 && std::memcmp(stream.data, "MDFNSVST", 8) == 0;
    const bool spuFirst = std::memcmp(stream.data + 32, "SPU", 3) == 0;
    const bool mdecSecond = std::memcmp(stream.data, "MDEC", 4) != 0; // the SPU chunk comes first
    if (!headerOk || !spuFirst || !mdecSecond) {
      error = std::string("Beetle MDFNSS_SaveSM wrote ") + std::to_string(stream.len) +
              " bytes that are not a header followed by an SPU chunk (magic/SPU-first)";
      return false;
    }
  }

  // 3. Move the device somewhere else, and PROVE the move changed the captured bytes. This is the
  //    step that keeps the test off the vacuous zero: if the fixture cannot move this device — or the
  //    fork's field list does not cover the registers the fixture writes — the rest of the sequence
  //    would compare identical bytes and report success while measuring nothing.
  perturbDevice(core, device, 0x005Au);
  BeetleDeviceState moved;
  if (!moved.save(core, device, error)) {
    return false;
  }
  const std::vector<std::uint8_t> elsewhere = moved.bytesOut();
  if (elsewhere == before) {
    error = std::string("Beetle ") + name +
            " captured byte-identical state at two different register values, so this round-trip "
            "would prove nothing — the fixture or the fork's field list is not covering this device";
    return false;
  }

  // 4. The fork's whole-stream reader must ACCEPT the stream its own writer produced. Acceptance is
  //    the claim; restoration through this driver is deliberately NOT claimed (see step 2).
  whole.rawStream().loc = 0; // MDFNSS_SaveSM leaves the cursor at byte 20, after patching the size
  if (MDFNSS_LoadSM(&whole.rawStream(), 0, 0) == 0) {
    error = std::string("Beetle MDFNSS_LoadSM refused the stream MDFNSS_SaveSM just wrote for ") + name;
    return false;
  }

  // 5. The claim production depends on: the SINGLE-CHUNK path restores the device. The device is
  //    still in the state step 3 moved it to (step 4 only ran the fork's whole-stream reader, whose
  //    restore is NOT the claim), so this load starts from a state the capture demonstrably differs
  //    from, and `chunkLoaded != elsewhere` afterwards is the load having taken effect.
  if (!captured.load(core, device, before, error)) {
    return false;
  }
  BeetleDeviceState afterChunk;
  if (!afterChunk.save(core, device, error)) {
    return false;
  }
  const std::vector<std::uint8_t> chunkLoaded = afterChunk.bytesOut();
  if (chunkLoaded == elsewhere) {
    error = std::string("Beetle ") + name +
            " is byte-identical to the state it was moved AWAY from, so the single-chunk load did not "
            "take effect";
    return false;
  }

  // 6. The FIXED POINT, which is the property a resumed run actually depends on: move it away again,
  //    restore `chunkLoaded`, and require the next capture to
  //    be exactly `chunkLoaded`. From here load and capture agree, so a run that loaded a state and
  //    saved one produces the bytes it loaded.
  perturbDevice(core, device, 0x0F0Fu);
  BeetleDeviceState third;
  if (!third.save(core, device, error)) {
    return false;
  }
  if (third.bytesOut() == chunkLoaded) {
    error = std::string("Beetle ") + name +
            " did not move under a second distinct perturbation, so the fixed-point check would pass "
            "without the load doing anything";
    return false;
  }
  if (!afterChunk.load(core, device, chunkLoaded, error)) {
    return false;
  }
  BeetleDeviceState fourth;
  if (!fourth.save(core, device, error)) {
    return false;
  }
  if (fourth.bytesOut() != chunkLoaded) {
    error = std::string("Beetle ") + name +
            " state is not a fixed point of load: " + std::to_string(chunkLoaded.size()) + " bytes loaded, " +
            std::to_string(fourth.size()) + " bytes captured back";
    return false;
  }
  return true;
}

} // namespace psx::state
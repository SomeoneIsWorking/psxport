#include "machine_state.h"

#include "beetle_device_state.h"
#include "core.h"
#include "device_sections.h"
#include "executable_write_source.h"
#include "game.h"
#include "game_runtime.h"
#include "invalidation.h"
#include "timing.h"

#include <lucent/log.h>

#include <algorithm>

namespace psx::state {
namespace {

// The title section's own layout: a u32 version, then the title's bytes. The framework writes the
// version so a refusal can name BOTH numbers rather than "the title section is wrong".

struct SectionWriter {
  StateImage &image;
  std::vector<std::string> &written;

  void put(std::string_view name, const BlobWriter &blob) {
    if (!image.add(name, blob.bytesOut())) {
      written.emplace_back(std::string(name) + " (refused)");
      return;
    }
    written.emplace_back(name);
  }
};

} // namespace

NativeStatePort *MachineState::titlePort(Game &game) {
  if (game.runtime == nullptr) {
    return nullptr;
  }
  return game.runtime->nativeState(game.core);
}

std::optional<std::vector<std::uint8_t>> MachineState::capture(std::string &error) {
  error.clear();
  Core &core = mGame.core;
  StateImage image;
  std::vector<std::string> written;

  auto emit = [&](std::string_view name, auto &&produce) {
    BlobWriter blob;
    produce(blob);
    SectionWriter{image, written}.put(name, blob);
  };

  emit(section::kCpu, [&](BlobWriter &out) {
    writeCpuSection(core, out);
  });
  emit(section::kRam, [&](BlobWriter &out) {
    writeRamSection(core, out);
  });
  emit(section::kScratchpad, [&](BlobWriter &out) {
    writeScratchpadSection(core, out);
  });
  emit(section::kGte, [&](BlobWriter &out) {
    writeGteSection(mGame, out);
  });
  emit(section::kGpu, [&](BlobWriter &out) {
    writeGpuSection(mGame, out);
  });

  // The vendored Beetle devices through the FORK'S OWN state functions, not a re-description of them.
  BeetleDeviceState spuState;
  std::string deviceError;
  if (!spuState.save(core, BeetleDevice::Spu, deviceError)) {
    error = deviceError;
    return std::nullopt;
  }
  {
    const auto bytes = spuState.bytesOut();
    BlobWriter blob;
    blob.bytes(bytes);
    SectionWriter{image, written}.put(section::kBeetleSpu, blob);
  }
  BeetleDeviceState mdecState;
  if (!mdecState.save(core, BeetleDevice::Mdec, deviceError)) {
    error = deviceError;
    return std::nullopt;
  }
  {
    const auto bytes = mdecState.bytesOut();
    BlobWriter blob;
    blob.bytes(bytes);
    SectionWriter{image, written}.put(section::kBeetleMdec, blob);
  }

  emit(section::kCdc, [&](BlobWriter &out) {
    writeCdcSection(mGame, out);
  });
  emit(section::kTiming, [&](BlobWriter &out) {
    writeTimingSection(mGame, out);
  });
  emit(section::kDma, [&](BlobWriter &out) {
    writeDmaSection(core, out);
  });
  emit(section::kHle, [&](BlobWriter &out) {
    writeHleSection(mGame, out);
  });
  emit(section::kSio, [&](BlobWriter &out) {
    writeSioSection(mGame, out);
  });

  {
    BlobWriter blob;
    writeCardSection(mGame, blob, error);
    if (!error.empty()) {
      return std::nullopt;
    }
    SectionWriter{image, written}.put(section::kCard, blob);
  }
  emit(section::kCd, [&](BlobWriter &out) {
    writeCdSection(mGame, out);
  });

  if (const NativeStatePort *port = titlePort(mGame)) {
    BlobWriter blob;
    blob.u8(kTitleEnvelopeVersion);
    blob.u32(port->version());
    if (!port->save(blob, error)) {
      error = "the title's native state refused to save: " + error;
      return std::nullopt;
    }
    // The framework's `title` section name is fixed (StateFile knows it); the port's own
    // sectionName() and version() are INSIDE the section, so the mismatch check is one read.
    if (!image.add(section::kTitle, blob.bytesOut())) {
      error = "the title section could not be appended";
      return std::nullopt;
    }
    written.emplace_back(section::kTitle);
  }

  // Every framework section must be present. A short list here would otherwise produce a file that
  // loads and leaves a device at power-on, which is indistinguishable from a state of a machine
  // that really was at power-on.
  const std::vector<std::string> required = {section::kCpu.data(),
                                             section::kRam.data(),
                                             section::kScratchpad.data(),
                                             section::kGte.data(),
                                             section::kGpu.data(),
                                             section::kBeetleSpu.data(),
                                             section::kBeetleMdec.data(),
                                             section::kCdc.data(),
                                             section::kTiming.data(),
                                             section::kDma.data(),
                                             section::kHle.data(),
                                             section::kSio.data(),
                                             section::kCard.data(),
                                             section::kCd.data()};
  for (const std::string &name : required) {
    if (std::find(written.begin(), written.end(), name) == written.end()) {
      error = "section '" + name +
              "' was not written; the image would restore that device at "
              "power-on and read as a legitimate state";
      return std::nullopt;
    }
  }
  return image.bytes();
}

std::optional<StateOutcome> MachineState::restore(std::span<const std::uint8_t> image, std::string &error) {
  error.clear();
  StateOutcome outcome;
  StateFile::OpenError openError;
  std::vector<std::uint8_t> owned(image.begin(), image.end());
  auto file = StateFile::open(std::move(owned), openError);
  if (!file) {
    error = openError.reason;
    return std::nullopt;
  }
  outcome.sections = file->names();
  outcome.bytes = image.size();
  outcome.fields = mGame.timing.vblank;

  // ---- the title check FIRST, while nothing has been mutated -----------------------------------
  NativeStatePort *port = titlePort(mGame);
  const auto titlePayload = file->titlePayload();
  if (port == nullptr && titlePayload.has_value()) {
    error = "this file carries a title state section and this title declares none "
            "(GameRuntime::nativeState(Core &) returns null); loading it would resume a guest whose "
            "native owners are at their power-on values";
    return std::nullopt;
  }
  if (port != nullptr && !titlePayload.has_value()) {
    error = std::string("this title owns native state (section '") + port->sectionName() + "' version " +
            std::to_string(port->version()) +
            ") and this file carries no title section; loading it "
            "would desync on the first field";
    return std::nullopt;
  }

  // ---- every required framework section present, BEFORE any mutation --------------------------
  const std::vector<std::string> required = {section::kCpu.data(),
                                             section::kRam.data(),
                                             section::kScratchpad.data(),
                                             section::kGte.data(),
                                             section::kGpu.data(),
                                             section::kBeetleSpu.data(),
                                             section::kBeetleMdec.data(),
                                             section::kCdc.data(),
                                             section::kTiming.data(),
                                             section::kDma.data(),
                                             section::kHle.data(),
                                             section::kSio.data(),
                                             section::kCard.data(),
                                             section::kCd.data()};
  for (const std::string &name : required) {
    if (!file->section(name).has_value()) {
      error = "the file carries " + std::to_string(outcome.sections.size()) + " sections and none of them is '" + name +
              "', which this machine needs";
      return std::nullopt;
    }
  }

  // The title's own envelope, read before anything else mutates so a version refusal costs nothing.
  if (port != nullptr) {
    BlobReader title(*titlePayload);
    const std::uint8_t envelope = title.u8();
    const std::uint32_t version = title.u32();
    if (!title.ok() || envelope != kTitleEnvelopeVersion) {
      error = "the title section envelope is " + std::to_string(envelope) + ", this build writes " +
              std::to_string(kTitleEnvelopeVersion);
      return std::nullopt;
    }
    if (version != port->version()) {
      error = std::string("the file's title state is section '") + port->sectionName() + "' version " +
              std::to_string(version) + "; this title writes version " + std::to_string(port->version());
      return std::nullopt;
    }
  }

  // ---- restore, in reverse capture order, control and timing LAST -----------------------------
  Core &core = mGame.core;
  auto read = [&](std::string_view name) {
    return BlobReader(*file->section(name));
  };

  if (port != nullptr) {
    BlobReader title(*titlePayload);
    title.u8();
    title.u32();
    if (!port->load(title, error)) {
      error = "the title's native state refused to load: " + error;
      return std::nullopt;
    }
  }

  {
    BlobReader in = read(section::kCd);
    if (!readCdSection(mGame, in, error)) {
      return std::nullopt;
    }
  }
  {
    // The card section WRITES the host card file, so it is restored after everything that could still
    // refuse — a refusal after this point would leave the operator's card holding a state that was
    // never loaded.
    BlobReader in = read(section::kCard);
    if (!readCardSection(mGame, in, error)) {
      return std::nullopt;
    }
  }
  {
    BlobReader in = read(section::kSio);
    if (!readSioSection(mGame, in, error)) {
      return std::nullopt;
    }
  }
  {
    BlobReader in = read(section::kHle);
    if (!readHleSection(mGame, in, error)) {
      return std::nullopt;
    }
  }
  {
    BlobReader in = read(section::kDma);
    if (!readDmaSection(core, in, error)) {
      return std::nullopt;
    }
  }
  {
    BlobReader in = read(section::kTiming);
    if (!readTimingSection(mGame, in, error)) {
      return std::nullopt;
    }
  }
  {
    BlobReader in = read(section::kCdc);
    if (!readCdcSection(mGame, in, error)) {
      return std::nullopt;
    }
  }
  {
    const auto payload = file->section(section::kBeetleMdec);
    BeetleDeviceState device;
    std::string deviceError;
    if (!device.load(core, BeetleDevice::Mdec, *payload, deviceError)) {
      error = deviceError;
      return std::nullopt;
    }
  }
  {
    const auto payload = file->section(section::kBeetleSpu);
    BeetleDeviceState device;
    std::string deviceError;
    if (!device.load(core, BeetleDevice::Spu, *payload, deviceError)) {
      error = deviceError;
      return std::nullopt;
    }
  }
  {
    BlobReader in = read(section::kGpu);
    if (!readGpuSection(mGame, in, error)) {
      return std::nullopt;
    }
  }
  {
    BlobReader in = read(section::kGte);
    if (!readGteSection(mGame, in, error)) {
      return std::nullopt;
    }
  }

  // RAM and the scratchpad last of the plain fields, then the ONE invalidation over both spans.
  // Reported to the single owner AFTER the bytes are visible: a block translated from the bytes being
  // replaced must never stay reachable, and a notification issued before the write would miss it.
  {
    BlobReader in = read(section::kScratchpad);
    if (!readScratchpadSection(core, in, error)) {
      return std::nullopt;
    }
  }
  {
    BlobReader in = read(section::kRam);
    if (!readRamSection(core, in, error)) {
      return std::nullopt;
    }
  }
  notifyExecutableWrite(core, {0x00000000u, 0x00200000u}, psx::cpu::ExecutableWriteSource::Savestate);
  notifyExecutableWrite(core, {0x1F800000u, 0x1F800400u}, psx::cpu::ExecutableWriteSource::Savestate);
  outcome.invalidationRanges = 2;
  {
    BlobReader in = read(section::kCpu);
    if (!readCpuSection(core, in, error)) {
      return std::nullopt;
    }
  }
  // Every section is in place and nothing below can refuse, so the title adopts what it staged.
  if (port != nullptr) {
    port->restored(core);
  }
  outcome.loaded = true;
  return outcome;
}

bool saveToFile(Core &core, const std::string &path, std::string &error) {
  MachineState state(*core.game);
  const auto image = state.capture(error);
  if (!image) {
    return false;
  }
  return writeFile(path, *image, error);
}

std::optional<StateOutcome> loadFromFile(Core &core, const std::string &path, std::string &error) {
  const auto bytes = readFile(path, error);
  if (!bytes) {
    return std::nullopt;
  }
  MachineState state(*core.game);
  auto outcome = state.restore(*bytes, error);
  if (outcome) {
    lucent::info("state",
                 "loaded {}: {} bytes, {} sections, field {}, {} invalidation ranges",
                 path,
                 outcome->bytes,
                 outcome->sections.size(),
                 outcome->fields,
                 outcome->invalidationRanges);
  }
  return outcome;
}

} // namespace psx::state
#include "machine_state.h"

#include "beetle_device_state.h"
#include "core.h"
#include "device_sections.h"
#include "executable_write_source.h"
#include "fs_util.h"
#include "game.h"
#include "game_runtime.h"
#include "guest_memory_map.h"
#include "invalidation.h"
#include "timing.h"

#include <lucent/log.h>

#include <algorithm>
#include <span>

namespace psx::state {
namespace {

using psx::platform::BlobReader;
using psx::platform::BlobWriter;

// The envelope ahead of a title's payload: this byte, then the title's own u32 version.
constexpr std::uint8_t kTitleEnvelopeVersion = 1;

struct SectionCodec {
  std::string_view name;
  bool (*write)(Game &, BlobWriter &, std::string &);
  bool (*read)(Game &, BlobReader &, std::string &);
};

bool writeBeetle(Game &game, BeetleDevice device, BlobWriter &out, std::string &error) {
  BeetleDeviceState state;
  if (!state.save(game.core, device, error)) {
    return false;
  }
  out.bytes(state.bytesOut());
  return true;
}

bool readBeetle(Game &game, BeetleDevice device, BlobReader &in, std::string &error) {
  std::vector<std::uint8_t> payload(in.size());
  in.bytes(payload);
  BeetleDeviceState state;
  return state.load(game.core, device, payload, error);
}

// Captured in this order and restored in reverse, so control and timing are restored last.
constexpr SectionCodec kSections[] = {
    {section::kCpu,
     [](Game &g, BlobWriter &out, std::string &) {
       writeCpuSection(g.core, out);
       return true;
     },
     [](Game &g, BlobReader &in, std::string &error) {
       return readCpuSection(g.core, in, error);
     }},
    {section::kRam,
     [](Game &g, BlobWriter &out, std::string &) {
       writeRamSection(g.core, out);
       return true;
     },
     [](Game &g, BlobReader &in, std::string &error) {
       if (!readRamSection(g.core, in, error)) {
         return false;
       }
       // The scratchpad is already restored; both spans are executable writes once visible.
       g.core.emission.clear();
       g.core.frameStates.clear();
       notifyExecutableWrite(g.core, {0x00000000u, psx::kMainRamBytes}, psx::cpu::ExecutableWriteSource::Savestate);
       notifyExecutableWrite(g.core,
                             {psx::kScratchpadPhysicalBase, psx::kScratchpadPhysicalBase + psx::kScratchpadBytes},
                             psx::cpu::ExecutableWriteSource::Savestate);
       return true;
     }},
    {section::kScratchpad,
     [](Game &g, BlobWriter &out, std::string &) {
       writeScratchpadSection(g.core, out);
       return true;
     },
     [](Game &g, BlobReader &in, std::string &error) {
       return readScratchpadSection(g.core, in, error);
     }},
    {section::kGte,
     [](Game &g, BlobWriter &out, std::string &) {
       writeGteSection(g, out);
       return true;
     },
     readGteSection},
    {section::kGpu,
     [](Game &g, BlobWriter &out, std::string &) {
       writeGpuSection(g, out);
       return true;
     },
     readGpuSection},
    {section::kBeetleSpu,
     [](Game &g, BlobWriter &out, std::string &error) {
       return writeBeetle(g, BeetleDevice::Spu, out, error);
     },
     [](Game &g, BlobReader &in, std::string &error) {
       return readBeetle(g, BeetleDevice::Spu, in, error);
     }},
    {section::kBeetleMdec,
     [](Game &g, BlobWriter &out, std::string &error) {
       return writeBeetle(g, BeetleDevice::Mdec, out, error);
     },
     [](Game &g, BlobReader &in, std::string &error) {
       return readBeetle(g, BeetleDevice::Mdec, in, error);
     }},
    {section::kCdc,
     [](Game &g, BlobWriter &out, std::string &) {
       writeCdcSection(g, out);
       return true;
     },
     readCdcSection},
    {section::kTiming,
     [](Game &g, BlobWriter &out, std::string &) {
       writeTimingSection(g, out);
       return true;
     },
     readTimingSection},
    {section::kDma,
     [](Game &g, BlobWriter &out, std::string &) {
       writeDmaSection(g.core, out);
       return true;
     },
     [](Game &g, BlobReader &in, std::string &error) {
       return readDmaSection(g.core, in, error);
     }},
    {section::kHle,
     [](Game &g, BlobWriter &out, std::string &) {
       writeHleSection(g, out);
       return true;
     },
     readHleSection},
    {section::kSio,
     [](Game &g, BlobWriter &out, std::string &) {
       writeSioSection(g, out);
       return true;
     },
     readSioSection},
    // Restoring the card writes the host card file, so it follows everything else that can refuse.
    {section::kCard,
     [](Game &g, BlobWriter &out, std::string &error) {
       writeCardSection(g, out, error);
       return error.empty();
     },
     readCardSection},
    {section::kCd,
     [](Game &g, BlobWriter &out, std::string &) {
       writeCdSection(g, out);
       return true;
     },
     readCdSection},
};

constexpr bool sectionsMatchRequired() {
  if (std::size(kSections) != section::kRequired.size()) {
    return false;
  }
  for (std::size_t i = 0; i < section::kRequired.size(); i++) {
    if (kSections[i].name != section::kRequired[i]) {
      return false;
    }
  }
  return true;
}
static_assert(sectionsMatchRequired(), "kSections is section::kRequired in order");

} // namespace

NativeStatePort *MachineState::titlePort(Game &game) {
  return game.runtime.nativeState(game.core);
}

std::optional<std::vector<std::uint8_t>> MachineState::capture(std::string &error) {
  error.clear();
  StateImage image;
  for (const SectionCodec &codec : kSections) {
    BlobWriter blob;
    if (!codec.write(mGame, blob, error)) {
      return std::nullopt;
    }
    if (!image.add(codec.name, blob.bytesOut())) {
      error = "section '" + std::string(codec.name) + "' could not be appended";
      return std::nullopt;
    }
  }
  if (const NativeStatePort *port = titlePort(mGame)) {
    BlobWriter blob;
    blob.u8(kTitleEnvelopeVersion);
    blob.u32(port->version());
    if (!port->save(blob, error)) {
      error = "the title's native state refused to save: " + error;
      return std::nullopt;
    }
    if (!image.add(section::kTitle, blob.bytesOut())) {
      error = "the title section could not be appended";
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

  // Every required section present, and the title envelope accepted, before anything mutates.
  std::array<std::span<const std::uint8_t>, std::size(kSections)> payloads{};
  for (std::size_t i = 0; i < std::size(kSections); i++) {
    const std::optional<std::span<const std::uint8_t>> found = file->section(kSections[i].name);
    if (!found.has_value()) {
      error = "the file carries " + std::to_string(outcome.sections.size()) + " sections and none of them is '" +
              std::string(kSections[i].name) + "', which this machine needs";
      return std::nullopt;
    }
    payloads[i] = *found;
  }
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
    if (!port->load(title, error)) {
      error = "the title's native state refused to load: " + error;
      return std::nullopt;
    }
  }
  for (std::size_t i = std::size(kSections); i-- > 0;) {
    BlobReader in(payloads[i]);
    if (!kSections[i].read(mGame, in, error)) {
      return std::nullopt;
    }
  }
  outcome.invalidationRanges = 2;
  Core &core = mGame.core;
  // Every section is in place and nothing below can refuse, so the title adopts what it staged.
  if (port != nullptr) {
    port->restored(core);
  }
  outcome.loaded = true;
  return outcome;
}

bool saveToFile(Core &core, const std::string &path, std::string &error) {
  error.clear();
  if (path.empty()) {
    error = "no path given";
    return false;
  }
  MachineState state(*core.game);
  const auto image = state.capture(error);
  if (!image || !Fs::writeFile(path, image->data(), image->size(), error)) {
    return false;
  }
  lucent::info("state",
               "saved {}: {} bytes, {} sections, field {}",
               path,
               image->size(),
               std::size(kSections) + (MachineState::titlePort(*core.game) != nullptr ? 1 : 0),
               core.game->timing.vblank);
  return true;
}

std::optional<StateOutcome> loadFromFile(Core &core, const std::string &path, std::string &error) {
  error.clear();
  if (path.empty()) {
    error = "no path given";
    return std::nullopt;
  }
  const auto bytes = Fs::readFile(path, error);
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
// Per-device section codecs; each owns its layout, writes what a later run can observe and refuses by name.
#pragma once

#include "blob_codec.h"

#include <string>

class Core;
class Game;

namespace psx::state {

// --- CPU: registers, COP0 and the interrupt-pending latch (device_cpu.cpp)
void writeCpuSection(Core &core, psx::platform::BlobWriter &out);
bool readCpuSection(Core &core, psx::platform::BlobReader &in, std::string &error);

// --- main RAM and the scratchpad (device_cpu.cpp); restoring either is an executable write.
void writeRamSection(Core &core, psx::platform::BlobWriter &out);
bool readRamSection(Core &core, psx::platform::BlobReader &in, std::string &error);
void writeScratchpadSection(Core &core, psx::platform::BlobWriter &out);
bool readScratchpadSection(Core &core, psx::platform::BlobReader &in, std::string &error);

// --- GTE: the 64 register words and FLAGS (device_cpu.cpp); OFX/OFY/H/DQA/DQB are CR[24..28].
void writeGteSection(Game &game, psx::platform::BlobWriter &out);
bool readGteSection(Game &game, psx::platform::BlobReader &in, std::string &error);

// --- the native GPU's guest-visible state and VRAM (device_gpu.cpp)
void writeGpuSection(Game &game, psx::platform::BlobWriter &out);
bool readGpuSection(Game &game, psx::platform::BlobReader &in, std::string &error);

// --- the device bus: CD controller, display clock, DMA controller, BIOS HLE/interrupt state,
// --- controller port + pad, memory card, and the native CD subsystem (device_bus.cpp)
void writeCdcSection(Game &game, psx::platform::BlobWriter &out);
bool readCdcSection(Game &game, psx::platform::BlobReader &in, std::string &error);
void writeTimingSection(Game &game, psx::platform::BlobWriter &out);
bool readTimingSection(Game &game, psx::platform::BlobReader &in, std::string &error);
void writeDmaSection(Core &core, psx::platform::BlobWriter &out);
bool readDmaSection(Core &core, psx::platform::BlobReader &in, std::string &error);
void writeHleSection(Game &game, psx::platform::BlobWriter &out);
bool readHleSection(Game &game, psx::platform::BlobReader &in, std::string &error);
void writeSioSection(Game &game, psx::platform::BlobWriter &out);
bool readSioSection(Game &game, psx::platform::BlobReader &in, std::string &error);
void writeCardSection(Game &game, psx::platform::BlobWriter &out, std::string &error);
bool readCardSection(Game &game, psx::platform::BlobReader &in, std::string &error);
void writeCdSection(Game &game, psx::platform::BlobWriter &out);
bool readCdSection(Game &game, psx::platform::BlobReader &in, std::string &error);

} // namespace psx::state
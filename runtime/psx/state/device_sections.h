// device_sections.h — the per-device section writers, one pair of functions per owner.
//
// Each function owns its device's private byte layout. The whole-machine owner (machine_state.h)
// decides WHICH sections exist and in what order; it does not know what is inside any of them, and
// no device writes a field another device owns. That is what keeps the state format from becoming a
// second, unowned description of the machine.
//
// A `write` must capture everything in its device that a LATER RUN CAN OBSERVE. A `read` restores
// exactly what its `write` captured, and refuses (by name, in `error`) rather than restoring a
// machine whose guest RAM, VRAM and devices disagree about which frame they are on.
#pragma once

#include "state_blob.h"

#include <string>

class Core;
class Game;

namespace psx::state {

// --- CPU: registers, COP0, the interrupt-pending latch, and the DMA channel shadows (device_cpu.cpp)
void writeCpuSection(Core &core, BlobWriter &out);
bool readCpuSection(Core &core, BlobReader &in, std::string &error);

// --- main RAM and the scratchpad (device_cpu.cpp). Restoring either is an EXECUTABLE WRITE, so the
// caller reports every differing range to the single invalidation owner afterwards.
void writeRamSection(Core &core, BlobWriter &out);
bool readRamSection(Core &core, BlobReader &in, std::string &error);
void writeScratchpadSection(Core &core, BlobWriter &out);
bool readScratchpadSection(Core &core, BlobReader &in, std::string &error);

// --- GTE: the 64 register words and FLAGS (device_cpu.cpp). That is the WHOLE of psxport's GTE
// state: Beetle's OFX/OFY/H/DQA/DQB are CR[24..28] of this same file (vendor/beetle-psx
// mednafen/psx/gte.c), so there is no second GTE state anywhere to capture.
void writeGteSection(Game &game, BlobWriter &out);
bool readGteSection(Game &game, BlobReader &in, std::string &error);

// --- the native GPU's guest-visible state and VRAM (device_gpu.cpp)
void writeGpuSection(Game &game, BlobWriter &out);
bool readGpuSection(Game &game, BlobReader &in, std::string &error);

// --- the device bus: CD controller, display clock, DMA controller, BIOS HLE/interrupt state,
// --- controller port + pad, memory card, and the native CD subsystem (device_bus.cpp)
void writeCdcSection(Game &game, BlobWriter &out);
bool readCdcSection(Game &game, BlobReader &in, std::string &error);
void writeTimingSection(Game &game, BlobWriter &out);
bool readTimingSection(Game &game, BlobReader &in, std::string &error);
void writeDmaSection(Core &core, BlobWriter &out);
bool readDmaSection(Core &core, BlobReader &in, std::string &error);
void writeHleSection(Game &game, BlobWriter &out);
bool readHleSection(Game &game, BlobReader &in, std::string &error);
void writeSioSection(Game &game, BlobWriter &out);
bool readSioSection(Game &game, BlobReader &in, std::string &error);
void writeCardSection(Game &game, BlobWriter &out, std::string &error);
bool readCardSection(Game &game, BlobReader &in, std::string &error);
void writeCdSection(Game &game, BlobWriter &out);
bool readCdSection(Game &game, BlobReader &in, std::string &error);

} // namespace psx::state
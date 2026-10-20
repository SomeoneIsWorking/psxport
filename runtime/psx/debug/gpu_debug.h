// gpu_debug.h — read-only text views of the GPU for the debug server and the REPL: the classified
// scene of an ordering table, and the display decision.
#pragma once

#include <cstdint>
#include <cstdio>

class Core;

namespace psx::debug {

// The ordering table at `madr` to the `scene` log channel.
void logScene(Core &core, std::uint32_t madr);
// The last ordering table DMA2 walked.
void dumpScene(Core &core, std::FILE *out);
// The display rect the device scans out and the draw area origin.
void dumpDisplay(Core &core, std::FILE *out);

} // namespace psx::debug

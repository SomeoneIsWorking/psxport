// arm64_translator.h — the AArch64 backend: one scanned guest block to host code.
#pragma once

#include "block_cache.h"
#include "block_scan.h"
#include "code_memory.h"

namespace psx::cpu::recompiler {

// Translates `block` (scanned with no refusal) into `memory`; null when the mapping has no room for it.
BlockCode translateArm64(const ScannedBlock &block, CodeMemory &memory);

} // namespace psx::cpu::recompiler

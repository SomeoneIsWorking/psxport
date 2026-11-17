// arm64_translator.h — the AArch64 backend: one scanned guest block to host code.
#pragma once

#include "block_cache.h"
#include "block_scan.h"
#include "code_memory.h"

namespace psx::cpu::recompiler {

// Translates `block` (scanned with no refusal) into `memory`; a null `code` when the mapping has no room for it.
// Every direct exit with no load in flight is a link site (block_cache.h LinkSite).
LinkedCode translateArm64(const ScannedBlock &block, CodeMemory &memory);
// Points `site` at a block's linked entry, or back at its own unlinked exit. The caller holds no WriteScope.
void patchLinkArm64(CodeMemory &memory, void *site, const void *linkedEntry);
void unpatchLinkArm64(CodeMemory &memory, void *site);

} // namespace psx::cpu::recompiler

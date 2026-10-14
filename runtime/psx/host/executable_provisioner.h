#pragma once

#include <cstdint>
#include <filesystem>

namespace psx::host {

struct TitleIdentity;

enum class Provisioning : std::uint8_t {
  Present,     // the file was already there; nothing was read or written
  Extracted,   // read from the title's disc, authenticated, written
  Unavailable, // still missing: no disc, no such file on it, or bytes that are not this title
};

// Extracts a title's boot executable (`discExecutablePath`) from its own disc into `target` when the
// file is missing. The bytes are authenticated against the title before anything is written.
Provisioning provisionExecutable(const TitleIdentity &title, const std::filesystem::path &target);

} // namespace psx::host

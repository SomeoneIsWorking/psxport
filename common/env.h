#pragma once
#include <optional>
#include <string>

namespace psxport {

// Resolve a config value: real environment variable first, then KEY=VALUE lines in a
// .env file found in the current dir or any parent (repo root when run from build/).
std::optional<std::string> GetEnv(const std::string& key);

// Resolve the disc image path: explicit arg (may be empty) > PSXPORT_DISC > first *.chd
// drop-in next to the .env / in the current dir. Empty optional if nothing found.
std::optional<std::string> ResolveDiscPath(const std::string& cli_arg);

}  // namespace psxport

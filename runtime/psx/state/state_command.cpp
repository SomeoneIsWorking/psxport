#include "state_command.h"

#include "config.h"
#include "config_vars.h"
#include "core.h"
#include "game.h"
#include "machine_state.h"
#include "timing.h"

#include <lucent/log.h>

#include <cstdio>
#include <cstring>

namespace psx::state {

bool save(Core &core, const std::string &path, std::string &error) {
  error.clear();
  if (path.empty()) {
    error = "no path given";
    return false;
  }
  MachineState state(*core.game);
  const auto image = state.capture(error);
  if (!image) {
    return false;
  }
  if (!writeFile(path, *image, error)) {
    return false;
  }
  // The section count and the size together are what make this reply checkable: "saved" alone
  // cannot be told from a file that silently lost a device.
  StateFile::OpenError openError;
  if (auto file = StateFile::open(*image, openError)) {
    std::string joined;
    for (const std::string &name : file->names()) {
      joined += joined.empty() ? "" : ",";
      joined += name;
    }
    lucent::info("state",
                 "saved {}: {} bytes, {} sections [{}], field {}",
                 path,
                 image->size(),
                 file->names().size(),
                 joined,
                 core.game->timing.vblank);
  } else {
    lucent::error("state", "the image just written to {} does not re-open: {}", path, openError.reason);
  }
  return true;
}

bool load(Core &core, const std::string &path, std::string &error) {
  error.clear();
  if (path.empty()) {
    error = "no path given";
    return false;
  }
  return loadFromFile(core, path, error).has_value();
}

bool handleControlCommand(Core &core, const char *line, void *out) {
  auto *stream = static_cast<FILE *>(out);
  char sub[32] = {0};
  char path[512] = {0};
  if (std::sscanf(line, "%31s %511s", sub, path) < 1 || std::strcmp(sub, "state") != 0) {
    return false;
  }
  char verb[32] = {0};
  if (std::sscanf(line, "%*s %31s %511s", verb, path) < 1) {
    std::fprintf(stream, "usage: state save <path> | state load <path>\n");
    return true;
  }
  std::string error;
  if (std::strcmp(verb, "save") == 0) {
    if (save(core, path, error)) {
      std::fprintf(stream, "state saved %s\n", path);
    } else {
      std::fprintf(stream, "state save REFUSED: %s\n", error.c_str());
      lucent::error("state", "state save {} REFUSED: {}", path, error);
    }
    return true;
  }
  if (std::strcmp(verb, "load") == 0) {
    if (load(core, path, error)) {
      std::fprintf(stream, "state loaded %s\n", path);
    } else {
      std::fprintf(stream, "state load REFUSED: %s\n", error.c_str());
      lucent::error("state", "state load {} REFUSED: {}", path, error);
    }
    return true;
  }
  std::fprintf(stream, "usage: state save <path> | state load <path>  (got '%s')\n", verb);
  return true;
}

bool applyConfiguredState(Core &core, std::string &error) {
  error.clear();
  // `TextVar::get()` returns BY VALUE, so the reference lifetime-extends the temporary; a
  // `const char *` bound to `.c_str()` of it would dangle before the first use.
  const std::string &path = psx::config::cv_load_state.get();
  if (path.empty()) {
    return true;
  }
  if (!load(core, path, error)) {
    error = "PSXPORT_LOAD_STATE=" + path + ": " + error;
    return false;
  }
  return true;
}

} // namespace psx::state
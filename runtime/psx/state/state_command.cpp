#include "state_command.h"

#include "config_vars.h"
#include "machine_state.h"

namespace psx::state {

bool applyConfiguredState(Core &core, std::string &error) {
  error.clear();
  const std::string path = psx::config::load_state_path();
  if (path.empty()) {
    return true;
  }
  if (!loadFromFile(core, path, error)) {
    error = "PSXPORT_LOAD_STATE=" + path + ": " + error;
    return false;
  }
  return true;
}

} // namespace psx::state

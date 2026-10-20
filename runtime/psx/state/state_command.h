// PSXPORT_LOAD_STATE, applied before the first field.
#pragma once

#include <string>

class Core;

namespace psx::state {

// False with `error` when PSXPORT_LOAD_STATE is set and the load refused.
bool applyConfiguredState(Core &core, std::string &error);

} // namespace psx::state

#pragma once

#include "executable_write_source.h"
#include "guest_address_range.h"

#include <cstddef>
#include <cstdint>

class Core;

namespace psx::cpu {

void notifyExecutableWrite(Core &core, GuestAddressRange range, ExecutableWriteSource source);
void notifyExecutableStateReplaced(Core &core, ExecutableWriteSource source);

} // namespace psx::cpu

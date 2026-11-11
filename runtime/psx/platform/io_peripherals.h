// io_peripherals.h — address decode for the interrupt controller, SIO0 and the root counters; false for any other
// address.
#pragma once
#include "guest_address_range.h"
#include <cstdint>
class Core;

namespace psx::platform {

bool readIoPeripheral(Core &core, uint32_t addr, uint32_t &out);
// Whether a read of `addr` is a root counter's value, which is derived from guest time and changes nothing.
bool readsGuestClock(uint32_t addr);

bool writeIoPeripheral(Core &core, GuestStore store);

} // namespace psx::platform

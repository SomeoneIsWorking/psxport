// io_peripherals.h — memory-mapped register routing for the interrupt controller, controller port
// (SIO0), and root counters, split out of mem.cpp so the address map has one owner instead of
// scattered switch arms in the memory unit. Behaviour lives with the subsystem that owns the state:
// interrupt delivery in Hle, pad protocol/deadlines in Sio0, and counters in psx::frame::Timing. This file is
// only the address decode between them.
//
// Both return false for an address that is none of theirs, so mem.cpp's unmapped-peripheral
// diagnostic still sees everything it used to.
#pragma once
#include "guest_address_range.h"
#include <cstdint>
class Core;

namespace psx::platform {

bool readIoPeripheral(Core &core, uint32_t addr, uint32_t &out);

bool writeIoPeripheral(Core &core, GuestStore store);

} // namespace psx::platform

// guest_pad_buffer_layout.h — immutable guest receive-buffer facts for host pad service.
#pragma once

#include <cstdint>

// The standard Sony pad packet is written to each resolved guest receive buffer. A title may expose
// fixed buffers, a driver-owned pointer table, or both. When both are present, a non-null driver
// pointer wins and the fixed address is the fallback, matching the legacy libpad integration.
struct GuestPadBufferLayout {
  uint32_t slot0Buffer = 0;
  uint32_t slot1Buffer = 0;
  uint32_t slotPointerTable = 0;
  uint32_t slotPointerStride = 4;
};

// guest_cd_stream_callback_layout.h — direct-runtime guest RAM facts for stock CD streaming.
#pragma once

#include <cstdint>

// The guest library owns the callback function value and may replace or clear it while running; the
// runtime supplies only the measured RAM slot that contains that value. Legacy consumers retain the
// equivalent GameConfig::cdReadyCbPtr fact while they migrate.
struct GuestCdStreamCallbackLayout {
  std::uint32_t readyCallbackPointer = 0;

  bool valid() const {
    return readyCallbackPointer != 0;
  }
};

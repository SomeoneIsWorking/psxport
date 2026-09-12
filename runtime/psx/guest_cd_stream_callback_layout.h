// guest_cd_stream_callback_layout.h — direct-runtime guest RAM facts for stock CD streaming.
#pragma once

#include <cstdint>

// A native stream consumer can own ready-callback dispatch when it also consumes the controller
// response (the legacy path). A stock libcd consumer instead receives INT1 through the guest BIOS
// interrupt path; its ISR consumes the response and then invokes the registered callback. Calling
// that callback from the pump first makes CdReady observe a stale libcd result.
// The guest library owns the callback function value and may replace or clear it while running; the
// runtime supplies the measured RAM slot and the source-grounded delivery owner. Legacy consumers
// retain GameConfig::cdReadyCbPtr and HostPump behavior while they migrate.
struct GuestCdStreamCallbackLayout {
  enum class DeliveryOwner : std::uint8_t { HostPump, GuestInterrupt };

  std::uint32_t readyCallbackPointer = 0;
  DeliveryOwner owner = DeliveryOwner::HostPump;

  bool valid() const {
    return readyCallbackPointer != 0;
  }
};

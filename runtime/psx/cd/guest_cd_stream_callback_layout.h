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

  // The first argument the guest's interrupt path hands the ready callback (`$a0`, the libcd
  // interrupt/completion code), for every delivery the framework makes on this title's behalf. The
  // default, 1, is libcd's data-ready code. A title whose callback branches on another code declares it,
  // read from the callback's own bytes (Spyro 2 and 3: `andi $v1,$a0,0xff` against 2, the completion
  // code that ends a read).
  std::uint8_t readyStatus = 1;

  // Does a stock `CdRead`, which the framework performs synchronously from the disc image, still owe the
  // guest the completion its interrupt handler would have delivered? A guest that chains one-sector
  // reads from the ready callback (Spyro 2 and 3) waits on it, and the synchronous read never touches the
  // controller, so without this the chain has nothing to start it. Meaningful only under
  // `DeliveryOwner::GuestInterrupt`; a title that declares nothing keeps its reads silent.
  bool stockReadRaisesCompletion = false;

  bool valid() const {
    return readyCallbackPointer != 0;
  }
};

// class Pad: the controller as the guest sees it (c->game->pad). It resolves host, forced, REPL and
// replayed input into one active-low mask per pad frame and delivers the digital packet; HostInput owns the host.
#pragma once
#include "active_low_edges.h"
#include "diagnostic_config.h"
#include "host_input.h"
#include "pad_record_replay.h"
#include <cstdint>
#include <vector>

class Core;
class Game;

class Pad {
public:
  explicit Pad(psx::input::HostInput &host) : mHost(host) {}

  Game *game = nullptr;
  uint16_t buttons = psx::input::kNoButtons;   // current effective mask, active-low (0 bit = pressed)
  uint16_t repl_hold = psx::input::kNoButtons; // REPL: bits cleared = held down
  uint16_t repl_tap = psx::input::kNoButtons;  // REPL: active-low mask pressed for repl_tap_n frames
  int repl_tap_n = 0;                          // REPL: tap countdown frames
  int repl_on = 0;                             // REPL drive active

  void init(); // nothing pressed

  // The one host-input pump: resolves the host mask and the P / '.' / B debug keys, advancing no frame.
  void pollHostInput();

  void driveHold(uint16_t activeLowMask);
  struct Tap {
    uint16_t activeLowMask = psx::input::kNoButtons;
    int frames = 0;
  };
  void driveTap(Tap tap);
  void driveRelease();

  // Once per pad frame: resolve every source into `buttons`, record or replay it, run the pad-frame
  // probes and deliver the packet. Advances the pad-frame clock.
  void serviceFrame();

  // Edges of the final mask; consumers decide what an edge means.
  void resetButtonEdges(uint16_t current = psx::input::kNoButtons) {
    mButtonEdges.reset(current);
  }
  bool pressedButton(uint16_t mask) const {
    return mButtonEdges.pressed(mask);
  }

  // The in-memory recording `padrec save` and bug reports cut from; nframes = 0 saves all.
  size_t recordedFrames() const {
    return mSession.recording().totalFrames();
  }
  bool saveRecording(const char *path, size_t nframes) const {
    return mSession.saveRecording(path, nframes);
  }
  const std::vector<std::uint8_t> &recordingStartCard() const {
    return mSession.startCardImage();
  }
  // A PSXPORT_PAD_RESUME replay still feeding the guest: the run is unpaced and unheard.
  bool fastForwarding() const {
    return mSession.fastForwarding();
  }

  // A host-only screen: live input, no recording sink and no replay. Call before the first serviceFrame().
  void useLiveInputOnly() {
    mLiveInputOnly = true;
  }
  // The host owns the player's input; the guest, the recording and a replay all see nothing pressed.
  void setPlayerInputSuppressed(bool suppressed) {
    mPlayerInputSuppressed = suppressed;
  }

private:
  psx::input::HostInput &mHost;
  ActiveLowEdges mButtonEdges;
  bool mForcedRead = false;
  psx::config::ForcedInput mForced;
  uint32_t mFc = 0; // pad frames serviced, the forced pulse's clock
  psx::input::PadRecordReplay mSession;
  bool mLiveInputOnly = false;
  bool mPlayerInputSuppressed = false;
};

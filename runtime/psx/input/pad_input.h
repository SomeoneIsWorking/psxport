// class Pad: the controller as the guest sees it (c->game->pad). It resolves host, forced and
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
  uint16_t buttons = psx::input::kNoButtons; // current effective mask, active-low (0 bit = pressed)

  void init(); // nothing pressed

  // The one host-input pump: resolves the host mask and the P / '.' / B debug keys, advancing no frame.
  void pollHostInput();

  // Once per pad frame: resolve every source into `buttons`, record or replay it, run the pad-frame
  // probes and deliver the packet. Advances the pad-frame clock.
  void serviceFrame();

  // This pad frame's mask from the keyboard and gamepads, the virtual one included, before forced and replayed input.
  uint16_t hostButtons() const {
    return mHostButtons;
  }

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
  // A pad replay is feeding the guest; it replaces live input, the virtual gamepad included.
  bool replaying() const {
    return mSession.replayPending();
  }
  // A PSXPORT_PAD_RESUME replay still feeding the guest: the run is unpaced and unheard.
  bool fastForwarding() const {
    return mSession.fastForwarding();
  }

  // A host-only screen: live input, no recording sink and no replay. Call before the first serviceFrame().
  void useLiveInputOnly() {
    mLiveInputOnly = true;
  }
  // The host owns the player's input; the guest, the recording and a replay all see nothing pressed,
  // the P / '.' / B debug keys do nothing here and this Game's overlay takes no host events.
  void setPlayerInputSuppressed(bool suppressed);

private:
  psx::input::HostInput &mHost;
  ActiveLowEdges mButtonEdges;
  bool mForcedRead = false;
  psx::config::ForcedInput mForced;
  uint32_t mFc = 0; // pad frames serviced, the forced pulse's clock
  uint16_t mHostButtons = psx::input::kNoButtons;
  psx::input::PadRecordReplay mSession;
  bool mLiveInputOnly = false;
  bool mPlayerInputSuppressed = false;
};

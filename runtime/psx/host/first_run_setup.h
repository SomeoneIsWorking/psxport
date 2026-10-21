#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace psx::host {

struct TitleIdentity;

// The screen that asks the player for a title's disc image when no key or saved choice names one
// (shared setup-ui). An accepted image is remembered through SavedDisc.
class FirstRunSetup {
public:
  enum class Outcome : std::uint8_t {
    Accepted,    // the player chose this title's disc and it was saved
    Declined,    // the player closed the screen
    Unavailable, // the window is not shown, or the screen could not open
  };

  explicit FirstRunSetup(const TitleIdentity &title);

  // Empty when `image` is a disc whose boot executable authenticates as this title; else why not.
  std::string judge(const std::filesystem::path &image) const;
  // Shown: the screen until the player accepts or declines. Hidden: one offscreen frame and a
  // refusal naming the disc key, so a tool run never waits on a player.
  Outcome run(bool shown) const;

private:
  const TitleIdentity &title_;
};

} // namespace psx::host

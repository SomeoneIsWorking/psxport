// The GameRuntime a test or tool builds a Game or Core around when it has no title of its own.
#pragma once

#include "game_runtime.h"

#include <string>

namespace psx::test {

// Declares nothing but a memory card of its own, so tests running side by side never share one. A test
// that needs one title virtual derives from it and overrides only that.
class StubGameRuntime : public psx::game::GameRuntime {
public:
  StubGameRuntime();
  ~StubGameRuntime() override;
  StubGameRuntime(const StubGameRuntime &) = delete;
  StubGameRuntime &operator=(const StubGameRuntime &) = delete;
  StubGameRuntime(StubGameRuntime &&) = delete;
  StubGameRuntime &operator=(StubGameRuntime &&) = delete;

  void registerOverrides(Game &game) override;
  RenderCapabilities renderCapabilities() const override;
  bool guestVramIsPicture(const Game &game) const override;
  const psx::game::HostIdentity *hostIdentity() const override;

private:
  std::string cardPath_;
  psx::game::HostIdentity identity_;
};

} // namespace psx::test

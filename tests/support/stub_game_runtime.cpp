#include "stub_game_runtime.h"

#include <cstdint>
#include <filesystem>
#include <format>
#include <random>
#include <system_error>

namespace psx::test {
namespace {

std::uint64_t uniqueToken() {
  std::random_device source;
  return (static_cast<std::uint64_t>(source()) << 32u) | source();
}

} // namespace

StubGameRuntime::StubGameRuntime()
    : cardPath_(std::format("scratch/test-cards/{:016x}.mcr", uniqueToken())),
      identity_{.cardDefaultPath = cardPath_.c_str()} {}

StubGameRuntime::~StubGameRuntime() {
  std::error_code ignored;
  std::filesystem::remove(cardPath_, ignored);
}

void StubGameRuntime::registerOverrides(Game &) {}

RenderCapabilities StubGameRuntime::renderCapabilities() const {
  return RenderCapabilities::direct();
}

bool StubGameRuntime::guestVramIsPicture(const Game &) const {
  return false;
}

const psx::game::HostIdentity *StubGameRuntime::hostIdentity() const {
  return &identity_;
}

} // namespace psx::test

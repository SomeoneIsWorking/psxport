// Beetle's own SPU/MDEC *_StateAction streams, bound to this machine's instance; GTE and GPU state are written elsewhere.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

class Core;

namespace psx::state {

// mednafen/state.h StateMem.
struct BeetleStateMem {
  std::uint8_t *data;
  std::uint32_t loc;
  std::uint32_t len;
  std::uint32_t malloced;
  std::uint32_t initial_malloc;
};

enum class BeetleDevice : std::uint8_t { Spu = 0, Mdec = 1 };

// Owns the fork's realloc'd stream, so it is neither copied nor moved.
class BeetleDeviceState {
public:
  BeetleDeviceState() = default;
  ~BeetleDeviceState();
  BeetleDeviceState(const BeetleDeviceState &) = delete;
  BeetleDeviceState &operator=(const BeetleDeviceState &) = delete;

  bool save(Core &core, BeetleDevice device, std::string &error);
  bool load(Core &core, BeetleDevice device, std::span<const std::uint8_t> payload, std::string &error);
  std::vector<std::uint8_t> bytesOut() const;
  void clear();

private:
  BeetleStateMem sm{};
};
} // namespace psx::state

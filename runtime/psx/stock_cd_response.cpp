#include "stock_cd_response.h"

#include "core.h"
#include "disc.h"
#include "game.h"
#include <array>
#include <cstdint>
#include <lucent/log.h>

namespace {

constexpr uint32_t kLeadInFrames = 150;
constexpr uint32_t kFramesPerSecond = 75;
constexpr uint32_t kSecondsPerMinute = 60;
using Response = std::array<uint8_t, 8>;

uint8_t to_bcd(uint32_t value) {
  return static_cast<uint8_t>(((value / 10u) << 4u) | (value % 10u));
}

bool from_bcd(uint8_t value, uint8_t &decoded) {
  if ((value & 0x0Fu) > 9 || (value >> 4u) > 9) {
    return false;
  }
  decoded = static_cast<uint8_t>((value >> 4u) * 10u + (value & 0x0Fu));
  return true;
}

bool valid_toc(const DiscState &disc) {
  if (disc.track_count == 0 || disc.track_count > DISC_MAX_TRACKS) {
    return false;
  }
  int64_t previous_end = -1;
  uint8_t previous_number = 0;
  for (uint8_t index = 0; index < disc.track_count; ++index) {
    const DiscTrackInfo &track = disc.tracks[index];
    const int64_t end = static_cast<int64_t>(track.lba) + track.sectors + track.postgap;
    if (track.number == 0 || track.number > 99 || track.number != previous_number + 1 || track.lba < 0 ||
        track.sectors == 0 || track.postgap < 0 || track.lba < previous_end || end > INT32_MAX) {
      return false;
    }
    previous_number = track.number;
    previous_end = end;
  }
  return true;
}

bool lba_to_msf(int64_t lba, uint8_t &minute, uint8_t &second) {
  const int64_t frames = lba + kLeadInFrames;
  if (frames < 0 || frames >= 100LL * kSecondsPerMinute * kFramesPerSecond) {
    return false;
  }
  const uint32_t total_seconds = static_cast<uint32_t>(frames / kFramesPerSecond);
  minute = to_bcd(total_seconds / kSecondsPerMinute);
  second = to_bcd(total_seconds % kSecondsPerMinute);
  return true;
}

bool toc_response(Core &core, uint8_t command, uint32_t parameter, Response &response) {
  DiscState &disc = core.game->disc;
  if (disc.track_count == 0 && !disc_open(&disc)) {
    return false;
  }
  if (!valid_toc(disc)) {
    return false;
  }
  response[0] = core.game->cdc.stat;
  if (command == 0x13) { // GetTN: first/last track numbers in BCD.
    response[1] = to_bcd(disc.tracks[0].number);
    response[2] = to_bcd(disc.tracks[disc.track_count - 1].number);
    return true;
  }
  if (!parameter) {
    return false;
  }
  uint8_t track_number = 0;
  if (!from_bcd(core.mem_r8(parameter), track_number)) {
    return false;
  }
  int64_t lba = -1;
  if (track_number == 0) { // GetTD(0) names the lead-out.
    const DiscTrackInfo &last = disc.tracks[disc.track_count - 1];
    lba = static_cast<int64_t>(last.lba) + last.sectors + last.postgap;
  } else {
    for (uint8_t index = 0; index < disc.track_count; ++index) {
      if (disc.tracks[index].number == track_number) {
        lba = disc.tracks[index].lba;
        break;
      }
    }
  }
  return lba >= 0 && lba_to_msf(lba, response[1], response[2]);
}

void write_response(Core &core, uint32_t result, const Response &response) {
  if (!result) {
    return;
  }
  for (uint32_t index = 0; index < response.size(); ++index) {
    core.mem_w8(result + index, response[index]);
  }
}

void publish(Core &core, uint32_t result) {
  const Cd &cd = core.game->cd;
  write_response(core, result, cd.stock_command_response_valid ? cd.stock_command_response : Response{});
}

} // namespace

void stock_cd_forget_response(Core &core) {
  Cd &cd = core.game->cd;
  cd.stock_command_response.fill(0);
  cd.stock_command_response_valid = false;
}

void stock_cd_zero_result(Core &core, uint32_t result) {
  write_response(core, result, Response{});
}

bool stock_cd_begin_command(Core &core, uint8_t command, uint32_t parameter, uint32_t result) {
  stock_cd_forget_response(core);
  if (command == 0x13 || command == 0x14) {
    Response response{};
    if (!toc_response(core, command, parameter, response)) {
      stock_cd_zero_result(core, result);
      lucent::error("cd", "stock command 0x{:02X} refused: missing or invalid disc TOC/track", command);
      return false;
    }
    core.game->cd.stock_command_response = response;
    core.game->cd.stock_command_response_valid = true;
  }
  publish(core, result);
  return true;
}

void stock_cd_publish_sync(Core &core, uint32_t result) {
  publish(core, result);
}

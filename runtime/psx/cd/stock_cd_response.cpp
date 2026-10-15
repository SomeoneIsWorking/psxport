#include "stock_cd_response.h"

#include "cdc_state.h"
#include "core.h"
#include "disc.h"
#include "disc_toc.h"
#include "game.h"
#include <array>
#include <cstdint>
#include <lucent/log.h>

namespace psx::cd {
namespace {

using Response = std::array<uint8_t, 8>;

bool toc_response(Core &core, StockCdCommand sent, Response &response) {
  const uint8_t command = sent.command;
  const uint32_t parameter = sent.parameter;
  psx::cd::Disc &disc = core.game->disc;
  psx::disc_toc::BcdPair answer{};
  if (command == 0x13) {
    if (!psx::disc_toc::track_range(disc, answer)) {
      return false;
    }
  } else if (!parameter || !psx::disc_toc::track_start(disc, core.mem_r8(parameter), answer)) {
    return false;
  }
  response[0] = core.game->cdc.stat;
  response[1] = answer.first;
  response[2] = answer.second;
  return true;
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

void forgetStockResponse(Core &core) {
  Cd &cd = core.game->cd;
  cd.stock_command_response.fill(0);
  cd.stock_command_response_valid = false;
}

void zeroStockResult(Core &core, uint32_t result) {
  write_response(core, result, Response{});
}

bool beginStockCommand(Core &core, StockCdCommand sent) {
  const uint8_t command = sent.command;
  const uint32_t result = sent.result;
  forgetStockResponse(core);
  if (command == 0x13 || command == 0x14) {
    Response response{};
    if (!toc_response(core, sent, response)) {
      zeroStockResult(core, result);
      lucent::error("cd", "stock command 0x{:02X} refused: missing or invalid disc TOC/track", command);
      return false;
    }
    core.game->cd.stock_command_response = response;
    core.game->cd.stock_command_response_valid = true;
  }
  if (command == 0x11) {
    Response response{};
    if (!core.game->cdc.getlocP(response.data())) {
      zeroStockResult(core, result);
      lucent::error("cd", "stock GetlocP refused: the disc layout is unavailable");
      return false;
    }
    core.game->cd.stock_command_response = response;
    core.game->cd.stock_command_response_valid = true;
  }
  publish(core, result);
  return true;
}

void publishStockSync(Core &core, uint32_t result) {
  publish(core, result);
}

} // namespace psx::cd

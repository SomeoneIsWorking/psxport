#include "stock_cd_response.h"

#include "cd.h"
#include "cdc_state.h"
#include "core.h"
#include "disc.h"
#include "disc_toc.h"
#include "game.h"
#include <cstdint>
#include <lucent/log.h>

namespace psx::cd {
namespace {

using Response = Cd::StockResponse::Bytes;

bool toc_response(Core &core, StockCdCommand sent, Response &response) {
  psx::disc_toc::BcdPair answer{};
  psx::cd::Disc &disc = core.game->disc;
  if (sent.command == 0x13) {
    if (!psx::disc_toc::track_range(disc, answer)) {
      return false;
    }
  } else if (!sent.parameter || !psx::disc_toc::track_start(disc, core.mem_r8(sent.parameter), answer)) {
    return false;
  }
  response[0] = core.game->cdc.stat;
  response[1] = answer.first;
  response[2] = answer.second;
  return true;
}

} // namespace

void zeroStockResult(Core &core, uint32_t result) {
  Cd::StockResponse{}.publish(core, result);
}

bool beginStockCommand(Core &core, StockCdCommand sent) {
  Cd::StockResponse &retained = core.game->cd.stockResponse;
  retained.clear();
  const bool toc = sent.command == 0x13 || sent.command == 0x14;
  if (toc || sent.command == 0x10 || sent.command == 0x11) {
    Response response{};
    CdController &drive = core.game->cdc;
    const bool answered = toc                    ? toc_response(core, sent, response)
                          : sent.command == 0x10 ? drive.getlocL(response.data())
                                                 : drive.getlocP(response.data());
    if (!answered) {
      zeroStockResult(core, sent.result);
      lucent::error("cd",
                    "stock command 0x{:02X} refused: {}",
                    sent.command,
                    sent.command == 0x10 ? "no sector header is latched"
                                         : "the disc TOC, track or layout is unavailable");
      return false;
    }
    retained.set(response);
  }
  retained.publish(core, sent.result);
  return true;
}

} // namespace psx::cd

void Cd::StockResponse::publish(Core &core, uint32_t result) const {
  if (!result) {
    return;
  }
  for (uint32_t index = 0; index < bytes_.size(); ++index) {
    core.mem_w8(result + index, bytes_[index]);
  }
}

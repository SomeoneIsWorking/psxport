#include "store_watch.h"

#include "core.h"
#include "diagnostic_config.h"
#include "game.h"
#include "guest_memory_map.h"
#include "host_backtrace.h"
#include "r3000.h"

#include <lucent/log.h>

namespace psx::debug {

void StoreWatch::configure() {
  const psx::config::StoreWatchConfig config = psx::config::store_watch();
  limit_ = config.limit;
  registers_ = config.registers;
  backtrace_ = config.backtrace;
  if (config.text.empty()) {
    return;
  }
  if (!config.range) {
    lucent::error("cw", "PSXPORT_CW='{}' is not a 'lo,hi' pair; no watch is armed", config.text);
    return;
  }
  arm(*config.range);
}

void StoreWatch::arm(GuestAddressRange range) {
  begin_ = physical(range.begin);
  end_ = physical(range.end);
  hits_ = 0;
  lucent::info("cw", "watch [{:08X},{:08X})", kseg0Address(begin_), kseg0Address(end_));
}

void StoreWatch::observe(Core &core, GuestStore store) {
  const std::uint32_t address = physical(store.address);
  if (address < begin_ || address >= end_) {
    return;
  }
  ++hits_;
  if (limit_ != 0 && hits_ > limit_) {
    return;
  }
  lucent::info("cw",
               "#{} f{} store w{} [{:08X}]={:08X} pc={:08X} ra={:08X} sp={:08X}",
               hits_,
               core.game ? core.game->display.frame() : 0,
               store.width,
               kseg0Address(address),
               store.value,
               core.pc,
               core.r[psx::cpu::kRa],
               core.r[psx::cpu::kSp]);
  if (registers_) {
    lucent::Line line;
    line.add("  gprs:");
    for (std::size_t i = 0; i < psx::cpu::kGprCount; ++i) {
      line.add(" {}={:08X}", psx::cpu::gprName(static_cast<std::uint32_t>(i)), core.r[i]);
    }
    line.flush(lucent::Level::Info, "cw");
  }
  if (backtrace_) {
    psxport::host::logBacktrace(32);
  }
}

} // namespace psx::debug

#include "painter_band_depth.h"

#include "census_frame.h"
#include "render_queue.h"

#include <lucent/log.h>
#include <stdlib.h>
#include <vector>

namespace {

struct BinBand {
  uint16_t bin = 0;
  float ord = 0.0f;
};

PainterBandDepthResult fail(PainterBandDepthResult out,
                            PainterBandDepthResult::Fault fault,
                            size_t command,
                            uint16_t bin,
                            float ord,
                            float other) {
  out.fault = fault;
  out.fault_command = command;
  out.fault_bin = bin;
  out.fault_ord = ord;
  out.fault_other = other;
  return out;
}

} // namespace

const char *painterBandDepthFaultName(PainterBandDepthResult::Fault fault) {
  switch (fault) {
  case PainterBandDepthResult::Fault::None:
    return "none";
  case PainterBandDepthResult::Fault::InconsistentBin:
    return "one OT bin carried two different band depths";
  case PainterBandDepthResult::Fault::NonMonotone:
    return "a bin drawn nearer was not given a nearer depth";
  case PainterBandDepthResult::Fault::DepthNotBanded:
    return "the submitted vertex depths are not the declared band";
  }
  return "<unknown>";
}

PainterBandDepthResult rq_check_painter_band_depths(std::span<const RqItem *const> stream,
                                                    const PainterObjectPlan &plan) {
  PainterBandDepthResult out{};
  std::vector<BinBand> seen;
  for (const PainterPlaybackRange &range : plan.ranges) {
    if (range.kind != PainterPlaybackKind::AuthoredDomain) {
      continue;
    }
    out.domains++;
    seen.clear();
    float farther_ord = 0.0f; // the previous bin's depth; the plan visits farther bins first
    bool have_previous = false;
    for (size_t k = 0; k < range.command_count; ++k) {
      const size_t command_index = range.first_command + k;
      const PainterCommand &command = plan.commands[command_index];
      out.commands_scanned++;
      if (!command.replay.banded()) {
        out.kept_own_depth++;
        continue;
      }
      const uint16_t bin = command.replay.key.ot_bin;
      const float ord = command.replay.band_ord;
      if (command.item_index >= stream.size() || stream[command.item_index] == nullptr) {
        return fail(out, PainterBandDepthResult::Fault::DepthNotBanded, command_index, bin, ord, 0.0f);
      }
      // The declaration is only worth having if the queue agrees with it. A producer that bands its
      // key and then submits a per-vertex depth would be ordered by the depth buffer exactly as
      // before, and the band would be a comment rather than a contract.
      const RqItem &item = *stream[command.item_index];
      for (int vertex = 0; vertex < 4; ++vertex) {
        if (item.depth[vertex] != ord) {
          return fail(out, PainterBandDepthResult::Fault::DepthNotBanded, command_index, bin, ord, item.depth[vertex]);
        }
      }
      // A bin is one depth. Two depths under one bin means the producer derived the band from
      // something that is not the bin, and picking either would order the frame by luck.
      bool known = false;
      for (const BinBand &band : seen) {
        if (band.bin != bin) {
          continue;
        }
        known = true;
        if (band.ord != ord) {
          return fail(out, PainterBandDepthResult::Fault::InconsistentBin, command_index, bin, ord, band.ord);
        }
        break;
      }
      if (!known) {
        // Larger depth is nearer under GREATER_OR_EQUAL, and the plan visits larger bins — farther —
        // first, so each new bin must be strictly nearer than the one before. An equal depth would
        // merge two bins the game kept apart, which is the same silent reordering this exists to stop.
        if (have_previous && !(ord > farther_ord)) {
          return fail(out, PainterBandDepthResult::Fault::NonMonotone, command_index, bin, ord, farther_ord);
        }
        seen.push_back({bin, ord});
        farther_ord = ord;
        have_previous = true;
        out.bins++;
      }
      out.banded++;
    }
  }
  return out;
}

void rq_verify_painter_band_depths(Core *core, std::span<const RqItem *const> stream, const PainterObjectPlan &plan) {
  const PainterBandDepthResult result = rq_check_painter_band_depths(stream, plan);
  if (!result.ok()) {
    lucent::error("rq",
                  "FATAL: painter band depth — {} (command {}, bin {}, ord {:.9f} vs {:.9f}); scanned {} "
                  "command(s) over {} authored domain(s), {} banded across {} bin(s), {} kept per-vertex depth",
                  painterBandDepthFaultName(result.fault),
                  result.fault_command,
                  result.fault_bin,
                  (double)result.fault_ord,
                  (double)result.fault_other,
                  result.commands_scanned,
                  result.domains,
                  result.banded,
                  result.bins,
                  result.kept_own_depth);
    abort();
  }
  // The denominator matters more than the count: a domain that declares no band is still ordered by
  // the depth buffer, and that has to be readable rather than inferred from silence.
  lucent::debug("painterband",
                "f{} {} authored domain(s): {} of {} command(s) banded across {} bin(s), {} kept per-vertex depth",
                census_frame(core),
                result.domains,
                result.banded,
                result.commands_scanned,
                result.bins,
                result.kept_own_depth);
}

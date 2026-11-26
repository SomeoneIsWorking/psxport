// psx::debug::DrawListReport — a bug report's draw list: every entry of the reported frame's record, where it lands
// in the screenshot and which guest code wrote it.
//
// The record is copied when its frame seals; the placement is taken at the present the screenshot shows, so a
// draw's `screen` box is in the same units as report.json's marks (fractions of screen.png).
#pragma once

#include "frame_record.h"
#include "present_plan.h"

#include <filesystem>
#include <optional>
#include <vector>

class Core;

namespace psx::debug {

class DrawListReport {
public:
  // The sealed frame's record.
  void capture(const psx::present::FrameRecord &record);
  // Where the screenshot's present put guest VRAM; none leaves every `screen` box out.
  void place(std::optional<psx::present::SinkPlacement> placement) {
    placement_ = placement;
  }
  bool captured() const {
    return captured_;
  }
  // One JSON line per record entry after a header line; false when the file cannot be written.
  bool write(Core &core, const std::filesystem::path &path) const;
  void clear();

private:
  std::vector<psx::present::RecordEntry> entries_;
  std::uint64_t sequence_ = 0;
  std::optional<psx::present::SinkPlacement> placement_;
  bool captured_ = false;
};

} // namespace psx::debug

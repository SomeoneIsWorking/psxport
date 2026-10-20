// function_reach — the distinct guest entry pcs a run dispatched, per code image (name and content
// identity, never generation). Disarmed by default; armed, a pc costs one bitmap test.
#pragma once

#include "image_identity.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace psx::cpu {

class FunctionReach {
public:
  // Writes an empty report at once, so a run that dies before any code runs still leaves one that says
  // it reached nothing, and rewrites it as it grows (kFlushEveryNewEntries), so a killed run leaves
  // everything but its last few entries, marked incomplete.
  FunctionReach(const ImageCatalog &catalog, std::string reportPath);
  ~FunctionReach();
  FunctionReach(const FunctionReach &) = delete;
  FunctionReach &operator=(const FunctionReach &) = delete;

  void observe(std::uint32_t guestPc);

  std::string reportJson(bool complete) const;

private:
  void flush();
  bool writeReport(bool complete) const;

  const ImageCatalog &catalog_;
  std::string reportPath_;
  std::vector<std::uint64_t> seenUnderRevision_;
  std::uint64_t revision_ = 0;
  // (image name, content identity) -> the distinct guest PCs reached inside that image.
  std::map<std::pair<std::string, std::uint64_t>, std::set<std::uint32_t>> reached_;
  std::uint64_t unownedDispatches_ = 0;
  std::uint64_t newSinceFlush_ = 0;
};

} // namespace psx::cpu

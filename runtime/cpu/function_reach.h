// function_reach — which guest code entries a run actually reached, per code image.
//
// Every guest PC the dynarec dispatches passes Lightrec's block boundary before lookup, compilation,
// fallback or execution (LightrecExecutor::Impl::blockBoundary), and every call target and native
// override entry is one of those PCs. Recording the distinct PCs there, keyed by the resident image
// that owned each one, answers "was this function reached on this route" for every function at once,
// without one probe per candidate. A title intersects the report with its own function list to state
// coverage as reached/total.
//
// The key is the image's NAME and CONTENT identity, never its load generation, so two runs that load the
// same overlay produce comparable reports and two overlays that reuse one address never merge.
//
// COST. Disarmed by default. Armed, a PC costs one bitmap test; only the first sighting of a PC under
// the current residency resolves its image.
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
  // it reached nothing.
  FunctionReach(const ImageCatalog &catalog, std::string reportPath);
  ~FunctionReach();
  FunctionReach(const FunctionReach &) = delete;
  FunctionReach &operator=(const FunctionReach &) = delete;

  void observe(std::uint32_t guestPc);

  // (image name, content identity) -> the distinct guest PCs reached inside that image.
  using Reached = std::map<std::pair<std::string, std::uint64_t>, std::set<std::uint32_t>>;
  const Reached &reached() const;
  std::uint64_t unownedDispatches() const;
  std::string reportJson(bool complete) const;

private:
  bool writeReport(bool complete) const;

  const ImageCatalog &catalog_;
  std::string reportPath_;
  std::vector<std::uint64_t> seenUnderRevision_;
  std::uint64_t revision_ = 0;
  Reached reached_;
  std::uint64_t unownedDispatches_ = 0;
};

} // namespace psx::cpu

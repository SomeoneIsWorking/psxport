#include "function_reach.h"

#include "fs_util.h"

#include <algorithm>
#include <lucent/log.h>
#include <optional>

namespace psx::cpu {

namespace {

// Main RAM is 2 MiB and instructions are word-aligned, so one bit per word covers every PC that can
// hold translated code. BIOS and scratchpad PCs fall outside it; those dispatches, and any to RAM no image owns, are
// counted, not recorded.
inline constexpr std::uint32_t kMainRamBytes = 0x200000u;
inline constexpr std::size_t kBitmapWords = kMainRamBytes / 4u / 64u;

std::string jsonEscaped(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    if (ch == '"' || ch == '\\') {
      out += '\\';
    }
    out += ch;
  }
  return out;
}

} // namespace

FunctionReach::FunctionReach(const ImageCatalog &catalog, std::string reportPath)
    : catalog_(catalog), reportPath_(std::move(reportPath)), seenUnderRevision_(kBitmapWords, 0u),
      revision_(catalog.revision()) {
  lucent::info("reach", "ARMED: recording every dispatched guest entry by image; report {}", reportPath_);
  (void)writeReport(false);
}

FunctionReach::~FunctionReach() {
  std::size_t entries = 0;
  for (const auto &[image, pcs] : reached_) {
    entries += pcs.size();
  }
  lucent::info("reach",
               "summary: {} distinct entry pc(s) across {} image(s); {} dispatch(es) to a pc outside every image",
               entries,
               reached_.size(),
               unownedDispatches_);
  (void)writeReport(true);
}

void FunctionReach::observe(std::uint32_t guestPc) {
  if (catalog_.revision() != revision_) {
    revision_ = catalog_.revision();
    std::fill(seenUnderRevision_.begin(), seenUnderRevision_.end(), 0u);
  }
  const std::uint32_t physical = guestPc & 0x1fffffffu;
  if (physical >= kMainRamBytes) {
    ++unownedDispatches_;
    return;
  }
  const std::uint32_t word = physical / 4u;
  const std::uint64_t bit = std::uint64_t{1} << (word % 64u);
  std::uint64_t &slot = seenUnderRevision_[word / 64u];
  if ((slot & bit) != 0u) {
    return;
  }
  slot |= bit;
  const std::optional<ImageIdentity> identity = catalog_.resolve(guestPc);
  const std::optional<ImageDescription> image = identity ? catalog_.describe(*identity) : std::nullopt;
  if (!image) {
    ++unownedDispatches_;
    return;
  }
  reached_[{image->name, image->contentIdentity}].insert(guestPc);
}

const FunctionReach::Reached &FunctionReach::reached() const {
  return reached_;
}

std::uint64_t FunctionReach::unownedDispatches() const {
  return unownedDispatches_;
}

std::string FunctionReach::reportJson(bool complete) const {
  std::string json = lucent::format(
      "{{\"complete\": {}, \"unowned_dispatches\": {}, \"images\": [", complete ? "true" : "false", unownedDispatches_);
  bool firstImage = true;
  for (const auto &[image, pcs] : reached_) {
    json += lucent::format("{}\n  {{\"name\": \"{}\", \"content\": \"0x{:016X}\", \"pcs\": [",
                           firstImage ? "" : ",",
                           jsonEscaped(image.first),
                           image.second);
    firstImage = false;
    bool firstPc = true;
    for (const std::uint32_t pc : pcs) {
      json += lucent::format("{}\"0x{:08X}\"", firstPc ? "" : ", ", pc);
      firstPc = false;
    }
    json += "]}";
  }
  json += "\n]}\n";
  return json;
}

bool FunctionReach::writeReport(bool complete) const {
  const std::string json = reportJson(complete);
  if (!Fs::writeFile(reportPath_, json.data(), json.size())) {
    lucent::error("reach", "could not write the report to {}", reportPath_);
    return false;
  }
  return true;
}

} // namespace psx::cpu

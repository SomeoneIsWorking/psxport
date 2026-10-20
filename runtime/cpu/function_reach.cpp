#include "function_reach.h"

#include "fs_util.h"
#include "guest_memory_map.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <lucent/log.h>
#include <lucent/text.h>
#include <optional>
#include <string>

namespace psx::cpu {

namespace {

// One bit per main-RAM word; dispatches outside main RAM or any image are counted, not recorded.
inline constexpr std::size_t kBitmapWords = kMainRamBytes / 4u / 64u;
// Rewritten after this many new entries and at every residency change, so a killed run leaves a report.
inline constexpr std::uint64_t kFlushEveryNewEntries = 64u;

// "0x" and `Digits` upper-case hex digits; to_chars cannot fail into a 16-character buffer.
template <std::size_t Digits> std::string hexLiteral(std::uint64_t value) {
  std::array<char, 16> buffer{};
  const auto end = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 16).ptr;
  const auto written = static_cast<std::size_t>(end - buffer.data());
  std::string out = "0x";
  out.append(Digits > written ? Digits - written : 0u, '0');
  for (const char *at = buffer.data(); at != end; ++at) {
    out += static_cast<char>(std::toupper(static_cast<unsigned char>(*at)));
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
    flush();
    std::fill(seenUnderRevision_.begin(), seenUnderRevision_.end(), 0u);
  }
  const std::uint32_t target = physical(guestPc);
  if (target >= kMainRamBytes) {
    ++unownedDispatches_;
    return;
  }
  const std::uint32_t word = target / 4u;
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
  if (reached_[{image->name, image->contentIdentity}].insert(guestPc).second &&
      ++newSinceFlush_ >= kFlushEveryNewEntries) {
    flush();
  }
}

void FunctionReach::flush() {
  if (newSinceFlush_ != 0u) {
    newSinceFlush_ = 0u;
    (void)writeReport(false);
  }
}

std::string FunctionReach::reportJson(bool complete) const {
  std::string json = "{\"complete\": ";
  json += complete ? "true" : "false";
  json += ", \"unowned_dispatches\": " + std::to_string(unownedDispatches_) + ", \"images\": [";
  bool firstImage = true;
  for (const auto &[image, pcs] : reached_) {
    json += firstImage ? "" : ",";
    json += "\n  {\"name\": " + lucent::text::json_quote(image.first) + ", \"content\": \"" +
            hexLiteral<16>(image.second) + "\", \"pcs\": [";
    firstImage = false;
    bool firstPc = true;
    for (const std::uint32_t pc : pcs) {
      json += firstPc ? "\"" : ", \"";
      json += hexLiteral<8>(pc) + "\"";
      firstPc = false;
    }
    json += "]}";
  }
  json += "\n]}\n";
  return json;
}

bool FunctionReach::writeReport(bool complete) const {
  const std::string json = reportJson(complete);
  std::string error;
  if (!Fs::writeFile(reportPath_, json.data(), json.size(), error)) {
    lucent::error("reach", "could not write the report: {}", error);
    return false;
  }
  return true;
}

} // namespace psx::cpu

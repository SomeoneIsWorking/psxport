#include "function_reach.h"

#include "fs_util.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <lucent/log.h>
#include <optional>
#include <string>

namespace psx::cpu {

namespace {

// Main RAM is 2 MiB and instructions are word-aligned, so one bit per word covers every PC that can
// hold translated code. BIOS and scratchpad PCs fall outside it; those dispatches, and any to RAM no image owns, are
// counted, not recorded.
inline constexpr std::uint32_t kMainRamBytes = 0x200000u;
inline constexpr std::size_t kBitmapWords = kMainRamBytes / 4u / 64u;
// A run that ends by being killed never reaches the destructor, so the report is also rewritten after
// this many new entries and at every residency change. It says `"complete": false` until the end.
inline constexpr std::uint64_t kFlushEveryNewEntries = 64u;

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

// "0x" and `digits` upper-case hex digits; to_chars cannot fail into a 16-character buffer.
std::string hexLiteral(std::uint64_t value, std::size_t digits) {
  std::array<char, 16> buffer{};
  const auto end = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 16).ptr;
  const auto written = static_cast<std::size_t>(end - buffer.data());
  std::string out = "0x";
  out.append(digits > written ? digits - written : 0u, '0');
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

const FunctionReach::Reached &FunctionReach::reached() const {
  return reached_;
}

std::uint64_t FunctionReach::unownedDispatches() const {
  return unownedDispatches_;
}

std::string FunctionReach::reportJson(bool complete) const {
  std::string json = "{\"complete\": ";
  json += complete ? "true" : "false";
  json += ", \"unowned_dispatches\": " + std::to_string(unownedDispatches_) + ", \"images\": [";
  bool firstImage = true;
  for (const auto &[image, pcs] : reached_) {
    json += firstImage ? "" : ",";
    json += "\n  {\"name\": \"" + jsonEscaped(image.first) + "\", \"content\": \"" + hexLiteral(image.second, 16) +
            "\", \"pcs\": [";
    firstImage = false;
    bool firstPc = true;
    for (const std::uint32_t pc : pcs) {
      json += firstPc ? "\"" : ", \"";
      json += hexLiteral(pc, 8) + "\"";
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

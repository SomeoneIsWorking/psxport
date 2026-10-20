// The versioned whole-machine container: "PSXSTATE", u32 version, u32 count, then per section a
// NUL-padded name[32], u32 length and payload. A name outside section::kRequired and `title` is refused.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psx::state {

inline constexpr char kStateMagic[8] = {'P', 'S', 'X', 'S', 'T', 'A', 'T', 'E'};
inline constexpr std::uint32_t kFormatVersion = 1;
inline constexpr std::size_t kSectionNameBytes = 32;
inline constexpr std::size_t kMaxSectionCount = 64;

namespace section {
inline constexpr std::string_view kCpu = "cpu";
inline constexpr std::string_view kRam = "ram";
inline constexpr std::string_view kScratchpad = "scratchpad";
inline constexpr std::string_view kGte = "gte";
inline constexpr std::string_view kGpu = "gpu";
inline constexpr std::string_view kBeetleSpu = "beetle.spu";
inline constexpr std::string_view kBeetleMdec = "beetle.mdec";
inline constexpr std::string_view kCdc = "cdc";
inline constexpr std::string_view kTiming = "timing";
inline constexpr std::string_view kDma = "dma";
inline constexpr std::string_view kHle = "hle";
inline constexpr std::string_view kSio = "sio";
inline constexpr std::string_view kCard = "card";
inline constexpr std::string_view kCd = "cd";
inline constexpr std::string_view kTitle = "title";
// Every device section a machine image carries, in capture order; `title` is the one optional name.
inline constexpr std::array<std::string_view, 14> kRequired{
    kCpu, kRam, kScratchpad, kGte, kGpu, kBeetleSpu, kBeetleMdec, kCdc, kTiming, kDma, kHle, kSio, kCard, kCd};
} // namespace section

// A written image: the bytes plus the section directory read back out of them.
class StateImage {
public:
  // Refuses an over-long or duplicate name.
  bool add(std::string_view name, std::span<const std::uint8_t> payload);
  const std::vector<std::uint8_t> &bytes() const {
    return mBytes;
  }
  std::vector<std::string> names() const;

private:
  std::vector<std::uint8_t> mBytes;
  std::vector<std::string> mNames;
};

// A parsed image. Constructed only by `open`, which is the only thing that validates the header.
class StateFile {
public:
  struct OpenError {
    std::string reason;
  };

  // Refuses, by name, a bad magic or version, a truncated table or payload, too many, duplicate or unknown sections.
  static std::optional<StateFile> open(std::vector<std::uint8_t> bytes, OpenError &error);

  [[nodiscard]] std::uint32_t version() const {
    return mVersion;
  }
  // Present exactly when the image carries a `title` section.
  [[nodiscard]] std::optional<std::span<const std::uint8_t>> titlePayload() const;
  std::optional<std::span<const std::uint8_t>> section(std::string_view name) const;
  std::vector<std::string> names() const {
    return mNames;
  }

private:
  struct Entry {
    std::string name;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
  };
  std::vector<std::uint8_t> mBytes;
  std::vector<Entry> mEntries;
  std::vector<std::string> mNames;
  std::uint32_t mVersion = kFormatVersion;
};

} // namespace psx::state
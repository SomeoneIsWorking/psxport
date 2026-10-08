// state_file.h — the ONE versioned container every whole-machine state is written through.
//
// A file is a header plus a flat sequence of named sections:
//
//   "PSXSTATE"            8 bytes
//   formatVersion        u32   (kFormatVersion)
//   sectionCount         u32
//   then, per section:  name[32] (NUL-padded), payloadLength u32, payload
//
// WHY SECTIONS AND NOT ONE BLOB. Two independent reasons, and the second is the one that costs
// money if it is forgotten. First, the title's own section is written by title code against a
// different version, so a version boundary has to exist somewhere the framework does not own.
// Second, a section table means a load can name the section it could not read instead of applying
// a partial machine: a file written by a newer framework, or by a title this build does not know,
// is refused with the section that differs rather than silently restoring some devices and not
// others.
//
// SECTION NAMES ARE A CONTRACT. `frameworkSectionNames()` is the complete list the framework owns;
// `StateFile` refuses to open an image carrying a name outside that list AND outside the one
// declared title section. An unknown section therefore always means "this file was written by
// something that knows state this build does not" — which is exactly the case where restoring the
// rest would produce a machine that never existed.
#pragma once

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

// The section names the framework itself writes. One list, owned here, because the loader has to be
// able to tell a framework section from a title section without the writer's cooperation.
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
} // namespace section

// Every framework section name, in one vector. Built once, so the answer cannot drift between the
// writer and the reader.
const std::vector<std::string> &frameworkSectionNames();
bool isFrameworkSectionName(std::string_view name);

// A written image: the bytes plus the section directory read back out of them.
class StateImage {
public:
  // Append a section. A name longer than kSectionNameBytes-1 is refused, and a duplicate name is
  // refused: two sections of one name would make the load order — and therefore the restored
  // machine — depend on which one the reader happened to find.
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

  // Parse `bytes`. Fails, by name, on: not a state file, a format version this build does not
  // write, a truncated section table, a section that runs past the end of the image, more sections
  // than kMaxSectionCount, a duplicate section name, or a section name this build does not know
  // (`title` excepted — that one is checked by the title port, which is the only thing that can
  // tell whether it is ITS section).
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

// Read/write the whole image. Both report failure by name rather than throwing: a state file that
// cannot be written is an operator problem, and the reply has to say which path and why.
std::optional<std::vector<std::uint8_t>> readFile(const std::string &path, std::string &error);
bool writeFile(const std::string &path, std::span<const std::uint8_t> bytes, std::string &error);

} // namespace psx::state
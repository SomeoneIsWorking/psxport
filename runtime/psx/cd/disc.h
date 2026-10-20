// disc.h — psx::cd::Disc: the title's CHD read by LBA, with a one-hunk cache and the parsed TOC. One per Game.
#pragma once
#include <libchdr/chd.h>
#include <optional>
#include <stdint.h>

namespace psx::cd {

inline constexpr int kDiscMaxTracks = 99;

// An ISO9660 file's first sector and byte size.
struct DiscFile {
  uint32_t lba = 0;
  uint32_t size = 0;
};

struct DiscTrackInfo {
  uint8_t number;
  int32_t lba;
  uint32_t sectors;
  int32_t pregap;
  int32_t pregap_dv;
  int32_t postgap;
};

// Parse one CHD metadata record and append its physical layout from `physical_lba`; exposed so metadata1/metadata2
// and malformed records can be falsified without constructing a CHD file.
bool parseTrackMetadata(const char *metadata, bool metadata2, int32_t *physical_lba, DiscTrackInfo *out_track);
// First *.chd in the working directory, NUL-terminated into out (disc_provision.cpp).
bool findDropinImage(char *out, unsigned out_cap);

class Disc {
public:
  Disc() = default;
  ~Disc();
  Disc(const Disc &) = delete;
  Disc &operator=(const Disc &) = delete;

  // Resolve the configured image (title key, PSXPORT_DISC, first-run choice, drop-in) and open it.
  bool open();
  // Opens exactly `path`, consulting no key; the first-run screen judges a chosen image with it.
  bool openImage(const char *path);
  // Closes the image and drops the cache and TOC; the next read opens it again.
  void close();
  bool isOpen() const {
    return chd != nullptr;
  }
  bool readSector(uint32_t lba, uint8_t *out);          // 2048-byte data sector
  bool readRaw(uint32_t lba, uint8_t *out, uint32_t n); // raw 2352-byte sector, n clamped to 2352
  // The eight GetlocP result bytes from the CHD's track layout: track, index, track-relative MM:SS:FF and
  // absolute MM:SS:FF, all BCD, as the vendored Beetle CHD backend synthesises them.
  bool subqPosition(uint32_t lba, uint8_t out[8]);
  // Print the hunk-cache denominators, whatever they are. `when` labels the moment.
  void readReport(const char *when) const;
  // Resolve an absolute ISO9660 path ("\BIN\START.BIN;1"; '/' accepted, case and ";version" ignored).
  std::optional<DiscFile> findFile(const char *path);

  chd_file *chd = nullptr;
  uint32_t frames_per_hunk = 0;
  uint32_t hunk_count = 0;
  uint32_t hunk_bytes = 0;
  uint8_t *hunk_buf = nullptr;
  uint32_t cached_hunk = 0xFFFFFFFFu; // cache empty
  // Cache denominators: hits alone cannot tell a working cache from one nothing asks.
  uint64_t lookups = 0;
  uint64_t hits = 0;
  uint64_t fills = 0;
  uint64_t fill_ns = 0;
  uint64_t worst_fill_ns = 0;
  const char *env_key = nullptr; // TitleIdentity::discEnvVar, set by psx::host::SavedDisc::bind
  char saved_path[1024] = {};    // the player's first-run choice (SavedDisc), read after the keys
  DiscTrackInfo tracks[kDiscMaxTracks] = {};
  uint8_t track_count = 0;

private:
  const uint8_t *residentHunk(uint32_t hunk, uint32_t lba, const char *reader);
};

} // namespace psx::cd

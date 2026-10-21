// disc.cpp — the title's CHD read by LBA through libchdr: 2048-byte user data and raw sectors for the
// CD controller, the stock libcd reads and a title's own CD overrides.
#include "disc.h"
#include "cd_position.h"
#include "config.h"
#include "host_clock.h"
#include "number_text.h"
#include <libchdr/chd.h>
#include <lucent/log.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace psx::cd {

Disc::~Disc() {
  close();
}

void Disc::close() {
  hunks.reset();
  if (chd) {
    chd_close(chd);
    chd = nullptr;
  }
  frames_per_hunk = 0;
  hunk_count = 0;
  hunk_bytes = 0;
  memset(tracks, 0, sizeof tracks);
  track_count = 0;
}

bool parseTrackMetadata(const char *metadata, bool metadata2, int32_t *physical_lba, DiscTrackInfo *out_track) {
  if (!metadata || !physical_lba || !out_track) {
    return false;
  }

  // CHT2 adds PREGAP PGTYPE PGSUB POSTGAP after the CHTR fields.
  constexpr std::array<std::string_view, 8> kKeys{
      "TRACK:", "TYPE:", "SUBTYPE:", "FRAMES:", "PREGAP:", "PGTYPE:", "PGSUB:", "POSTGAP:"};
  const std::size_t keyCount = metadata2 ? 8 : 4;
  std::array<std::string_view, 8> values{};
  std::string_view rest(metadata);
  for (std::size_t i = 0; i < keyCount; i++) {
    rest.remove_prefix(std::min(rest.find_first_not_of(" \t\r\n"), rest.size()));
    const std::size_t end = std::min(rest.find_first_of(" \t\r\n"), rest.size());
    const std::string_view field = rest.substr(0, end);
    rest.remove_prefix(end);
    if (!field.starts_with(kKeys[i]) || field.size() == kKeys[i].size()) {
      return false;
    }
    values[i] = field.substr(kKeys[i].size());
  }
  const std::optional<int> parsed_track_number = psx::config::parseInt(values[0]);
  const std::optional<int> parsed_frames = psx::config::parseInt(values[3]);
  const std::optional<int> parsed_pregap = metadata2 ? psx::config::parseInt(values[4]) : std::optional<int>(0);
  const std::optional<int> parsed_postgap = metadata2 ? psx::config::parseInt(values[7]) : std::optional<int>(0);
  if (!parsed_track_number.has_value() || !parsed_frames.has_value() || !parsed_pregap.has_value() ||
      !parsed_postgap.has_value() || *parsed_track_number <= 0 || *parsed_track_number > 99 || *parsed_frames <= 0 ||
      *parsed_pregap < 0 || *parsed_postgap < 0 || (values[1] != "MODE2_RAW" && values[1] != "AUDIO") ||
      values[2] != "NONE") {
    return false;
  }

  const int track_number = *parsed_track_number;
  const int frames = *parsed_frames;
  const int pregap = *parsed_pregap;
  const int postgap = *parsed_postgap;
  const int pregap_in_file = metadata2 && values[5].starts_with('V') ? pregap : 0;
  if (frames < pregap_in_file) {
    return false;
  }
  DiscTrackInfo track{};
  track.number = static_cast<uint8_t>(track_number);
  track.pregap = track_number == 1 ? kLeadInSectors : (pregap_in_file ? 0 : pregap);
  track.pregap_dv = pregap_in_file;
  const int64_t start = static_cast<int64_t>(*physical_lba) + track.pregap + track.pregap_dv;
  track.sectors = static_cast<uint32_t>(frames - track.pregap_dv);
  track.postgap = postgap;
  const int64_t next = start + track.sectors + track.postgap;
  if (start < INT32_MIN || start > INT32_MAX || next < INT32_MIN || next > INT32_MAX) {
    return false;
  }
  track.lba = static_cast<int32_t>(start);
  *physical_lba = static_cast<int32_t>(next);
  *out_track = track;
  return true;
}

namespace {

constexpr uint32_t kRawFrame = kRawSectorBytes + 96u; // raw sector + subcode, the CHD CD unit

uint32_t readLe32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// An ISO9660 record name (length `nlen`, no NUL) against path component `want`; case and ";version" ignored.
bool isoNameEquals(const uint8_t *name, int nlen, const char *want) {
  int i = 0;
  for (; i < nlen; i++) {
    char nc = (char)name[i];
    char wc = want[i];
    if (nc == ';') {
      break;
    }
    if (wc == ';' || wc == 0) {
      return false;
    }
    char a = (nc >= 'a' && nc <= 'z') ? (char)(nc - 32) : nc;
    char b = (wc >= 'a' && wc <= 'z') ? (char)(wc - 32) : wc;
    if (a != b) {
      return false;
    }
  }
  return want[i] == 0 || want[i] == ';';
}

} // namespace

// The game's key then PSXPORT_DISC, from the environment then ./.env; then the first-run choice; then a drop-in.
bool Disc::open() {
  if (chd) {
    return true;
  }
  std::string path = psx::config::launch_setting({env_key, "PSXPORT_DISC"});
  if (path.empty() && saved_path[0] != '\0') {
    path = saved_path;
  }
  if (path.empty()) {
    char buf[512];
    if (findDropinImage(buf, sizeof buf)) {
      path = buf;
    }
  }
  if (path.empty()) {
    lucent::warn("disc",
                 "no disc image: tried {}, PSXPORT_DISC (env and ./.env), the first-run choice and a "
                 "*.chd drop-in in the working directory. The CD model will run with NO MEDIA.",
                 env_key ? env_key : "<no TitleIdentity::discEnvVar>");
    return false;
  }
  return openImage(path.c_str());
}

bool Disc::openImage(const char *path) {
  const uint64_t started = psx::platform::hostNowNs();
  if (chd_open(path, CHD_OPEN_READ, 0, &chd) != CHDERR_NONE) {
    lucent::error("disc", "failed to open CHD: {}", path);
    return false;
  }
  const chd_header *h = chd_get_header(chd);
  hunk_bytes = h->hunkbytes;
  frames_per_hunk = h->hunkbytes / kRawFrame;
  hunk_count = h->totalhunks;
  if (frames_per_hunk == 0) {
    lucent::error("disc", "invalid CHD hunk geometry: {}", path);
    close();
    return false;
  }
  memset(tracks, 0, sizeof tracks);
  track_count = 0;
  int32_t physical_lba = -kLeadInSectors;
  uint32_t metadata_index = 0;
  while (metadata_index < kDiscMaxTracks) {
    char metadata[256] = {};
    uint32_t metadata_length = 0;
    chd_error error = chd_get_metadata(
        chd, CDROM_TRACK_METADATA2_TAG, metadata_index, metadata, sizeof metadata, &metadata_length, nullptr, nullptr);
    bool metadata2 = error == CHDERR_NONE;
    if (!metadata2) {
      error = chd_get_metadata(
          chd, CDROM_TRACK_METADATA_TAG, metadata_index, metadata, sizeof metadata, &metadata_length, nullptr, nullptr);
      if (error != CHDERR_NONE) {
        break;
      }
    }
    DiscTrackInfo track{};
    if (!parseTrackMetadata(metadata, metadata2, &physical_lba, &track)) {
      lucent::error("disc", "invalid or unsupported CHD track metadata {}: {}", metadata_index, metadata);
      close();
      return false;
    }
    tracks[track_count] = track;
    ++track_count;
    ++metadata_index;
  }
  if (track_count == 0) {
    lucent::error("disc", "CHD contains no CD track metadata");
    close();
    return false;
  }
  hunks = std::make_unique<HunkReader>(chd, path);
  lucent::info("disc",
               "opened {} ({} hunks, {} frames/hunk) in {} us",
               path,
               hunk_count,
               frames_per_hunk,
               (psx::platform::hostNowNs() - started) / 1000);
  return true;
}

bool Disc::subqPosition(uint32_t lba, uint8_t out[8]) {
  if (!out || (track_count == 0 && !open())) {
    return false;
  }

  const int64_t position = lba;
  const DiscTrackInfo *track = &tracks[0];
  for (uint8_t index = 0; index < track_count; ++index) {
    const DiscTrackInfo &candidate = tracks[index];
    const int64_t begin = static_cast<int64_t>(candidate.lba) - candidate.pregap_dv - candidate.pregap;
    const int64_t end = static_cast<int64_t>(candidate.lba) + candidate.sectors + candidate.postgap;
    if (position >= begin && position < end) {
      track = &candidate;
      break;
    }
  }

  const uint32_t relative =
      static_cast<uint32_t>(position >= track->lba ? position - track->lba : track->lba - position);
  const std::array<uint8_t, 3> trackMsf = sectorsToMsf(relative);
  const std::array<uint8_t, 3> absoluteMsf = lbaToMsf(static_cast<int>(lba));
  out[0] = toBcd(track->number);
  out[1] = toBcd(position < track->lba ? 0u : 1u);
  std::copy(trackMsf.begin(), trackMsf.end(), out + 2);
  std::copy(absoluteMsf.begin(), absoluteMsf.end(), out + 5);
  return true;
}

// The one place a hunk is read; it counts lookups, hits (decoded ahead), fills and the caller's time on fills.
const uint8_t *Disc::residentHunk(uint32_t hunk, uint32_t lba, const char *reader) {
  lookups++;
  const uint64_t started = psx::platform::hostNowNs();
  const HunkReader::Fetch fetched = hunks->hunk(hunk);
  if (fetched.resident) {
    hits++;
    return fetched.data;
  }
  const uint64_t elapsed = psx::platform::hostNowNs() - started;
  fills++;
  fill_ns += elapsed;
  if (elapsed > worst_fill_ns) {
    worst_fill_ns = elapsed;
  }
  // The hunk sequence is the working set the read-ahead is sized from.
  lucent::debug("dischunk",
                "fill hunk={} lba={} by={} us={} lookups={} fills={}",
                hunk,
                lba,
                reader,
                elapsed / 1000,
                lookups,
                fills);
  return fetched.data;
}

void Disc::readReport(const char *when) const {
  lucent::info("disc",
               "{}: {} hunk lookup(s), {} hit(s), {} fill(s) ({:.1f}% miss), {:.1f} ms waiting on fills, "
               "worst fill {:.1f} ms",
               when,
               lookups,
               hits,
               fills,
               lookups ? 100.0 * (double)fills / (double)lookups : 0.0,
               (double)fill_ns / 1e6,
               (double)worst_fill_ns / 1e6);
}

bool Disc::readSector(uint32_t lba, uint8_t *out) {
  if (!chd && !open()) {
    return false;
  }
  uint32_t hunk = lba / frames_per_hunk;
  uint32_t off = (lba % frames_per_hunk) * kRawFrame;
  if (hunk >= hunk_count) {
    lucent::info("disc", "LBA {} out of range", lba);
    return false;
  }
  const uint8_t *hunk_base = residentHunk(hunk, lba, "sector");
  if (!hunk_base) {
    return false;
  }
  const uint8_t *raw = hunk_base + off;
  // Mode-2 sectors carry an 8-byte subheader before user data.
  const uint32_t data_off = isMode2(raw) ? 24 : 16;
  memcpy(out, raw + data_off, kDataSectorBytes);
  return true;
}

// The raw sector keeps the Mode2 subheader (raw[16..23]) that tells XA audio from MDEC video.
bool Disc::readRaw(uint32_t lba, uint8_t *out, uint32_t n) {
  if (!chd && !open()) {
    return false;
  }
  if (n > kRawSectorBytes) {
    n = kRawSectorBytes;
  }
  uint32_t hunk = lba / frames_per_hunk;
  uint32_t off = (lba % frames_per_hunk) * kRawFrame;
  if (hunk >= hunk_count) {
    return false;
  }
  const uint8_t *hunk_base = residentHunk(hunk, lba, "raw");
  if (!hunk_base) {
    return false;
  }
  memcpy(out, hunk_base + off, n);
  return true;
}

// Walks the directory tree from the Primary Volume Descriptor; the native CdSearchFile.
std::optional<DiscFile> Disc::findFile(const char *path) {
  if (!chd && !open()) {
    return std::nullopt;
  }
  uint8_t sec[kDataSectorBytes];
  // Primary Volume Descriptor at sector 16; its root directory record sits at offset 156 (34 bytes).
  if (!readSector(16, sec)) {
    return std::nullopt;
  }
  if (sec[0] != 1) {
    return std::nullopt;
  }
  uint32_t dir_lba = readLe32(sec + 156 + 2);
  uint32_t dir_size = readLe32(sec + 156 + 10);

  const char *p = path;
  while (*p == '\\' || *p == '/') {
    p++;
  }
  while (*p) {
    char comp[64];
    int cn = 0;
    while (*p && *p != '\\' && *p != '/' && cn < (int)sizeof(comp) - 1) {
      comp[cn++] = *p++;
    }
    comp[cn] = 0;
    while (*p == '\\' || *p == '/') {
      p++;
    }
    const bool is_last = (*p == 0);

    // Records never cross a 2048-byte sector boundary.
    uint32_t nsec = (dir_size + kDataSectorBytes - 1u) / kDataSectorBytes;
    bool found = false;
    for (uint32_t s = 0; s < nsec && !found; s++) {
      if (!readSector(dir_lba + s, sec)) {
        return std::nullopt;
      }
      uint32_t o = 0;
      while (o < kDataSectorBytes) {
        uint32_t rlen = sec[o];
        if (rlen == 0 || o + rlen > kDataSectorBytes) {
          break;
        }
        int nlen = sec[o + 32];
        if (isoNameEquals(sec + o + 33, nlen, comp)) {
          uint32_t e_lba = readLe32(sec + o + 2);
          uint32_t e_size = readLe32(sec + o + 10);
          if (is_last) {
            return DiscFile{e_lba, e_size};
          }
          dir_lba = e_lba;
          dir_size = e_size;
          found = true;
          break;
        }
        o += rlen;
      }
    }
    if (!found) {
      return std::nullopt;
    }
  }
  return std::nullopt; // path ended on a directory, not a file
}

} // namespace psx::cd

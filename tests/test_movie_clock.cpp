// The movie clock: sector period from the XA interleave, frame due times from the frame slot, and
// whole-field holds on the display grid. Layouts are the shapes of Tomba! 2's LOGO.STR and Toy Story
// 2's TT.STR.
#include "fmv_decode.h"
#include "movie_clock.h"
#include "testutil.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace {

using psx::movie::MovieClock;
using psx::movie::MovieFieldGrid;
using psx::movie::SectorFacts;
using psx::movie::XaFormat;

constexpr double kNtscFieldMs = 1000000.0 / 59940.0;

bool near(double got, double want) {
  return std::fabs(got - want) < 1e-6;
}

// A constant-rate STR: frame n owns sectors [(n-1)*slot, n*slot), and every `stride`-th sector from
// `firstAudio` is XA audio of `audio` format.
struct Layout {
  std::uint32_t slot = 0;
  std::uint32_t stride = 0;
  std::uint32_t firstAudio = 0;
  XaFormat audio;

  bool isAudio(std::uint32_t sector) const {
    return stride != 0 && sector >= firstAudio && (sector - firstAudio) % stride == 0;
  }
  SectorFacts facts(std::uint32_t sector) const {
    SectorFacts out;
    if (isAudio(sector)) {
      out.audio = audio;
    } else {
      out.videoFrame = static_cast<int>(sector / slot) + 1;
    }
    return out;
  }
  // The last video sector of frame `frame`.
  std::uint32_t lastChunk(int frame) const {
    std::uint32_t sector = static_cast<std::uint32_t>(frame) * slot - 1;
    while (isAudio(sector)) {
      --sector;
    }
    return sector;
  }
  MovieClock learned() const {
    MovieClock clock;
    for (std::uint32_t sector = 0; sector < MovieClock::kLearnSectors && !clock.learned(); ++sector) {
      clock.learn(facts(sector));
    }
    return clock;
  }
};

// Tomba! 2 LOGO.STR: 37.8 kHz stereo XA every 8th sector from sector 7, five sectors per frame.
const Layout kLogo{5, 8, 7, {37800, 2016}};
// Toy Story 2 TT.STR: XA every 16th sector from sector 15, six sectors per frame.
const Layout kTt{6, 16, 15, {37800, 4032}};

std::array<std::uint8_t, 2352> xaSector(std::uint8_t coding) {
  std::array<std::uint8_t, 2352> raw{};
  raw[18] = 0x64; // audio, form 2, real-time
  raw[19] = coding;
  return raw;
}

void test_xa_format_comes_from_the_coding_byte() {
  const XaFormat stereo = psx::movie::xaSectorFormat(xaSector(0x01).data());
  CHECK_EQ(stereo.rate, 37800);
  CHECK_EQ(stereo.frames, 2016);
  const XaFormat mono = psx::movie::xaSectorFormat(xaSector(0x00).data());
  CHECK_EQ(mono.frames, 4032);
  const XaFormat half = psx::movie::xaSectorFormat(xaSector(0x05).data());
  CHECK_EQ(half.rate, 18900);
  CHECK_EQ(half.frames, 2016);
  CHECK_EQ(psx::movie::xaSectorFormat(xaSector(0x11).data()).frames, 1008);
  // The decoder writes as many frames as the format says.
  std::array<std::int16_t, std::size_t{4032} * 2> pcm{};
  std::int16_t hist[2][2] = {{0, 0}, {0, 0}};
  int rate = 0;
  CHECK_EQ(psx::movie::decodeXaSector(xaSector(0x00).data(), pcm.data(), hist, &rate), 4032);
  CHECK_EQ(rate, 37800);
}

void test_the_sector_period_is_one_xa_sectors_audio_over_its_stride() {
  const MovieClock logo = kLogo.learned();
  CHECK(logo.learned());
  CHECK(near(logo.sectorMs(), 2016.0 * 1000.0 / (37800.0 * 8.0)));
  CHECK(logo.firstAudioSector() == 7u);
  const MovieClock tt = kTt.learned();
  CHECK(near(tt.sectorMs(), 4032.0 * 1000.0 / (37800.0 * 16.0)));
  CHECK_EQ(tt.slotSectors(), 6);
}

void test_a_silent_movie_streams_at_double_speed() {
  MovieClock clock;
  for (std::uint32_t sector = 0; sector < 40; ++sector) {
    clock.learn({{}, static_cast<int>(sector / 4) + 1});
  }
  CHECK(near(clock.sectorMs(), 1000.0 / 150.0));
  CHECK_EQ(clock.slotSectors(), 4);
}

// The frames between two XA sectors used to share one due time; each now has its own, one slot apart,
// whether or not an XA sector sits in its slot.
void test_frames_are_due_evenly_across_xa_sectors() {
  for (const Layout &layout : {kLogo, kTt}) {
    const MovieClock clock = layout.learned();
    const double periodMs = layout.slot * clock.sectorMs();
    double previous = 0.0;
    int audioSectorsInSlots = 0;
    for (int frame = 1; frame <= 400; ++frame) {
      const double due = clock.frameDueMs({frame, layout.lastChunk(frame)});
      CHECK(near(due - previous, periodMs));
      // Due once its last chunk has been read.
      CHECK(due >= clock.sectorDeliveredMs(layout.lastChunk(frame)) - 1e-9);
      for (std::uint32_t sector = (frame - 1) * layout.slot; sector < frame * layout.slot; ++sector) {
        audioSectorsInSlots += layout.isAudio(sector) ? 1 : 0;
      }
      previous = due;
    }
    CHECK(audioSectorsInSlots > 0);
  }
}

// A 25 fps movie on the 59.94 Hz grid holds frames for two and three fields, and the holds add up
// to exactly the fields the movie spans.
void test_each_frame_is_held_for_the_whole_fields_to_its_successor() {
  const MovieClock clock = kTt.learned();
  MovieFieldGrid grid(kNtscFieldMs);
  int fields = 0;
  for (int frame = 1; frame <= 300; ++frame) {
    const double due = clock.frameDueMs({frame, kTt.lastChunk(frame)});
    const MovieFieldGrid::Show show = grid.show(due);
    CHECK(show.atMs >= due - 1e-9);
    CHECK(show.atMs < due + kNtscFieldMs);
    CHECK(show.heldFields == 2 || show.heldFields == 3);
    fields += show.heldFields;
  }
  const double endMs = clock.frameDueMs({300, kTt.lastChunk(300)});
  CHECK_EQ(fields, static_cast<int>(std::ceil(endMs / kNtscFieldMs - 1e-9)));
}

void test_two_frames_never_share_a_field() {
  MovieFieldGrid grid(kNtscFieldMs);
  CHECK_EQ(grid.show(10.0).heldFields, 1);
  const MovieFieldGrid::Show second = grid.show(12.0);
  CHECK_EQ(second.heldFields, 1);
  CHECK(near(second.atMs, 2 * kNtscFieldMs));
}

} // namespace

int main() {
  RUN(xa_format_comes_from_the_coding_byte);
  RUN(the_sector_period_is_one_xa_sectors_audio_over_its_stride);
  RUN(a_silent_movie_streams_at_double_speed);
  RUN(frames_are_due_evenly_across_xa_sectors);
  RUN(each_frame_is_held_for_the_whole_fields_to_its_successor);
  RUN(two_frames_never_share_a_field);
  return psx::test::summary();
}

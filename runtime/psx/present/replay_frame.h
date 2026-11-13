// replay_frame.h — a title's 60 fps in-betweens drawn by its own draw section, replayed with blended inputs.
//
// Before each real run of the section, the motion set samples what moves; when the frame follows the last one,
// the section runs again in a GuestRedraw once per in-between k/n of the interval, with the inputs written k/n
// of the way, and the packet chain it linked is captured. At seal the captures belong to the sealed record S.
// An in-between is S with the section's chain run replaced by the capture, decoded under S's draw state at that run.
#pragma once

#include "frame_record.h"
#include "gp0_draw_state.h"
#include "motion_set.h"
#include "palette_cycle.h"
#include "record_cluts.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

class Core;

namespace psx::present {

// The title's draw section, run inside a replay after the inputs are written.
class ReplaySection {
public:
  virtual ~ReplaySection() = default;
  // The section's steps; the head of the chain it linked, or nullopt when a step was refused.
  [[nodiscard]] virtual std::optional<std::uint32_t> draw(Core &core, GuestRedraw &redraw) const = 0;
  // Whether a chain the frame walked from `head` is the one the section links; its first packet moves with the
  // scene, so this names the frame's other chains out.
  [[nodiscard]] virtual bool linked(std::uint32_t head) const = 0;
};

class ReplayFrame {
public:
  // At most this many in-betweens a logic frame: a longer frame is a stall, shown as drawn.
  static constexpr int kMaxInBetweens = 3;

  ReplayFrame(const ReplaySection &section,
              std::span<const MotionTrack> tracks,
              std::span<MotionSource *const> sources = {},
              const PaletteCycles *cycles = nullptr);

  // The section is about to run for real: samples the inputs and, when they pair, replays the in-betweens;
  // nothing while fps60 is off.
  void beforeDraw(Core &core);
  // The frame did not run the section, or broke from the last (cut, load): the next sample pairs with nothing.
  void forget();
  // FramePresenter sealed `sequence`; a drawing record takes the captures made since the last.
  void sealed(std::uint64_t sequence, bool draws);

  // An interval the presenter shows: S' = `from` to S = `shown` over `fields` fields.
  struct Interval {
    std::uint64_t shown = 0;
    std::uint64_t from = 0;
    int fields = 0;
  };
  // Whether S = `shown`'s draw ran the section; an S that did not is shown as drawn.
  [[nodiscard]] bool replays(std::uint64_t shown) const;
  // Whether the interval's in-betweens were replayed, for its `fields`.
  [[nodiscard]] bool covers(const Interval &interval) const;
  // In-between `k` of S' = `from` → S = `shown`, assembled; null when S's chain run cannot take it.
  [[nodiscard]] const FrameRecord *
  at(const FrameRecord &shown, const FrameRecord &from, int k, std::span<const std::uint16_t> vram);
  // The fraction the section is being replayed at, while it runs: a title override inside the replay draws that
  // in-between of what it emits there.
  [[nodiscard]] std::optional<float> replaying() const {
    return replaying_;
  }
  // The tracks' samples, for a title override that places its draw by a blended input.
  [[nodiscard]] const MotionSet &motion() const {
    return motion_;
  }
  // Run end: intervals replayed (and their in-betweens) and refused by the section, in-betweens shown and held at
  // assembly, then each track's and source's unpaired items over the replayed intervals.
  void report() const;

private:
  struct Capture {
    std::vector<gpu::CapturedPacket> packets;
    std::vector<std::uint32_t> words;
    std::vector<gte::WordPlace> places;
    RecordWindows windows;                // frameWindows inside the replay, so the in-between frames its own view
    std::vector<std::uint32_t> uiPackets; // the packets the replay drew as crop UI (cropShowsUi), ascending
    // The packets the replay drew outline-font glyphs with and each one's font (OutlineGlyphs), by packet.
    std::vector<std::pair<std::uint32_t, std::uint8_t>> glyphPackets;
  };
  struct Set {
    std::optional<std::uint64_t> shown;
    std::optional<std::uint64_t> from;
    int fields = 0;
    bool ran = false;
    std::vector<Capture> captures; // k = 1..fields-1
    CycledRows cycles;
  };
  // One replay at `t`; false when the section refused or its chain could not be read.
  bool replay(Core &core, float t, Capture &capture);
  // In-between `k` of S from its capture; null, logged, when S's chain run cannot take it.
  [[nodiscard]] const FrameRecord *assemble(const FrameRecord &shown,
                                            const RecordCluts::Between &between,
                                            const Capture &capture,
                                            int k,
                                            std::span<const std::uint16_t> vram);
  // The set that S = `shown` took, or null.
  [[nodiscard]] const Set *setFor(std::uint64_t shown) const;

  const ReplaySection &section_;
  MotionSet motion_;
  const PaletteCycles *cycles_;
  // The display field of the last sample.
  std::optional<std::uint32_t> sampledField_;
  std::optional<float> replaying_;
  Set pending_;
  bool pendingReady_ = false;
  // The section ran since the last drawing seal.
  bool ran_ = false;
  std::optional<std::uint64_t> lastDrawing_;
  std::array<Set, 3> sets_;
  std::size_t nextSet_ = 0;
  // One per k: the presenter holds the last present's in-between while it assembles the earlier ones.
  std::array<FrameRecord, kMaxInBetweens> assembled_;
  // The record each of assembled_ was assembled for since the last seal, and whether it was held.
  std::array<std::optional<std::uint64_t>, kMaxInBetweens> assembledFor_{};
  std::array<bool, kMaxInBetweens> assembledHeld_{};
  RecordCluts cluts_;
  std::vector<RecordEntry> decoded_;
  // Intervals.
  std::uint64_t replayed_ = 0;
  std::uint64_t refused_ = 0;
  // In-betweens.
  std::uint64_t replayedInBetweens_ = 0;
  std::uint64_t shownInBetweens_ = 0;
  std::uint64_t held_ = 0;
};

} // namespace psx::present

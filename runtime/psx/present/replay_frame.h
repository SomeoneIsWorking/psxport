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
#include "record_cluts.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
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
              std::span<MotionSource *const> sources = {});

  // The section is about to run for real: samples the inputs and, when they pair, replays the in-betweens.
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
  // Whether S = `shown`'s draw ran the section; an S that did not is composed from producer states, if any.
  [[nodiscard]] bool replays(std::uint64_t shown) const;
  // Whether the interval's in-betweens were replayed. The next set is replayed for its `fields`.
  [[nodiscard]] bool covers(const Interval &interval);
  // In-between `k` of S, assembled; null when S's chain run cannot take it.
  [[nodiscard]] const FrameRecord *at(const FrameRecord &shown, int k, std::span<const std::uint16_t> vram);

private:
  struct Capture {
    std::vector<gpu::CapturedPacket> packets;
    std::vector<std::uint32_t> words;
    std::vector<gte::WordPlace> places;
  };
  struct Set {
    std::optional<std::uint64_t> shown;
    std::optional<std::uint64_t> from;
    int fields = 0;
    bool ran = false;
    std::vector<Capture> captures; // k = 1..fields-1
  };
  // One replay at `t`; false when the section refused or its chain could not be read.
  bool replay(Core &core, float t, Capture &capture);
  // The set that S = `shown` took, or null.
  [[nodiscard]] const Set *setFor(std::uint64_t shown) const;

  const ReplaySection &section_;
  MotionSet motion_;
  // The interval length the next set is replayed for: the last one the presenter asked for.
  int fields_ = 2;
  Set pending_;
  bool pendingReady_ = false;
  // The section ran since the last drawing seal.
  bool ran_ = false;
  std::optional<std::uint64_t> lastDrawing_;
  std::array<Set, 3> sets_;
  std::size_t nextSet_ = 0;
  // One per k: the presenter holds the last present's in-between while it assembles the earlier ones.
  std::array<FrameRecord, kMaxInBetweens> assembled_;
  RecordCluts cluts_;
  std::vector<RecordEntry> decoded_;
};

} // namespace psx::present

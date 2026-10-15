// picture_announce — the per-Core statement of the picture geometry a run resolved, on every change,
// and a warning when a wide picture was requested and did not happen (a run that silently failed to
// widen is otherwise indistinguishable from one that widened).
#pragma once

#include <cstdint>

#include "mods.h" // ASPECT_4_3

class Core;

namespace psx::picture {

// What this run renders. The -1 initial state makes the first present a change.
struct Geometry {
  int aspect = -1;
  int nativeWidth = -1;
  int renderWidth = -1;

  bool operator==(const Geometry &) const = default;
};

// Emits `[wide] native picture: aspect=... native_width=... render_width=...` when this Core's geometry
// differs from the last line emitted for it. `presentedFramebufferWidth` is the width handed to the
// presenter this call; render_width is derived from it by the presenter's own psx::present::presentDisplayWidth.
void announceOnChange(Core &core, int presentedFramebufferWidth);

enum class WideOutcome : std::uint8_t {
  NotRequested,        // 4:3: nothing to say
  Widened,             // render_width > native_width
  RefusedNoProjection, // the title declares no guest widescreen projection
  RefusedDevice,       // RenderPath::Device presents the device's own 4:3 picture
  RefusedAuto,         // ASPECT_AUTO resolves to the sink, and this run had no wide sink
  RefusedUnexplained   // requested and possible, and the width still did not grow
};

// `aspect` is the requested aspect (the title's own, else Mods::aspect), as an ASPECT_* value.
WideOutcome classifyWide(int aspect, bool titleProjects, bool recordPath, int nativeWidth, int renderWidth);

// The reason for a refused outcome in one sentence; empty for the two quiet outcomes.
const char *wideOutcomeReason(WideOutcome outcome);

} // namespace psx::picture

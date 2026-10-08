#include "render_queue_attribution.h"

#include "host_backtrace.h"
#include <cstdlib>
#include <execinfo.h>
#include <lucent/log.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace psxport::render {

namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void mix(std::uint64_t &hash, std::uint64_t value) {
  for (int byte = 0; byte < 8; ++byte) {
    hash ^= (value >> (byte * 8)) & 0xFFull;
    hash *= kFnvPrime;
  }
}

std::uint64_t floatBits(float value) {
  std::uint32_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value), "float must be 32 bits for a bit-exact identity");
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

const char *layerName(int layer) {
  switch (layer) {
  case RQ_BACKGROUND:
    return "background";
  case RQ_WORLD:
    return "world";
  case RQ_OVERLAY:
    return "overlay";
  case RQ_HUD:
    return "hud";
  default:
    return "?";
  }
}

} // namespace

// Position, texture and owner. Colour and depth are deliberately EXCLUDED: a re-submission loop that
// walks the same geometry twice can still shade it differently on the second pass (a lighting or fade
// term advanced in between), and counting those as distinct would hide exactly the runaway this exists
// to name. Two genuinely different faces of a scene differ in position, so this cannot merge them.
std::uint64_t RenderQueueAttribution::geometryKey(const RqItem &item) {
  std::uint64_t hash = kFnvOffset;
  mix(hash, item.nv);
  mix(hash, item.layer);
  mix(hash, item.dbg_node);
  for (int v = 0; v < item.nv && v < 4; ++v) {
    // World prims carry SUB-PIXEL float positions and the integer xs/ys are the rounded copy. Keying on
    // the rounded copy alone would merge two genuinely different faces that round to the same pixel, and
    // every such merge is counted as a repeat — which would manufacture the runaway verdict out of
    // nothing. Use the float positions where they are the authority, bit-exactly (this asks "did the
    // same submitter push this same face again", not "are these two faces nearly equal").
    if (item.has_xyf) {
      mix(hash, floatBits(item.xsf[v]));
      mix(hash, floatBits(item.ysf[v]));
    } else {
      mix(hash, static_cast<std::uint64_t>(static_cast<std::uint32_t>(item.xs[v])));
      mix(hash, static_cast<std::uint64_t>(static_cast<std::uint32_t>(item.ys[v])));
    }
    mix(hash, static_cast<std::uint64_t>(static_cast<std::uint32_t>(item.us[v])));
    mix(hash, static_cast<std::uint64_t>(static_cast<std::uint32_t>(item.vs[v])));
  }
  return hash;
}

RenderQueueAttribution::RenderQueueAttribution(const RqItem *items, int count) {
  if (items == nullptr || count <= 0) {
    return;
  }
  mPrims = count;

  std::unordered_set<std::uint64_t> seen;
  seen.reserve(static_cast<std::size_t>(count) * 2u);
  std::unordered_map<std::uint32_t, QueueNodeShare> byNode;

  for (int i = 0; i < count; ++i) {
    const RqItem &item = items[i];
    if (item.layer < RQ_LAYER_COUNT) {
      mByLayer[item.layer]++;
    }
    const bool first = seen.insert(geometryKey(item)).second;
    if (first) {
      mDistinct++;
    }
    QueueNodeShare &share = byNode[item.dbg_node];
    share.node = item.dbg_node;
    share.prims++;
    share.distinct += first ? 1 : 0;
  }

  mNodes = static_cast<int>(byNode.size());
  mTopNodes.reserve(byNode.size());
  for (const auto &entry : byNode) {
    mTopNodes.push_back(entry.second);
  }
  // Ties broken by node id so the report is reproducible across runs; an unordered_map's iteration
  // order is not, and a fatal whose text changes between identical runs cannot be diffed.
  std::sort(mTopNodes.begin(), mTopNodes.end(), [](const QueueNodeShare &a, const QueueNodeShare &b) {
    if (a.prims != b.prims) {
      return a.prims > b.prims;
    }
    return a.node < b.node;
  });
  if (static_cast<int>(mTopNodes.size()) > kTopNodes) {
    mTopNodes.resize(kTopNodes);
  }
}

int RenderQueueAttribution::primsInLayer(int layer) const {
  if (layer < 0 || layer >= RQ_LAYER_COUNT) {
    return 0;
  }
  return mByLayer[layer];
}

std::string RenderQueueAttribution::text() const {
  std::string out;
  out += "  " + std::to_string(mPrims) + " prim(s) examined: " + std::to_string(mDistinct) +
         " geometrically distinct, " + std::to_string(repeats()) +
         " repeating geometry already submitted "
         "this frame, across " +
         std::to_string(mNodes) + " owning node(s).\n";
  out += "  by layer:";
  for (int layer = 0; layer < RQ_LAYER_COUNT; ++layer) {
    out += " " + std::string(layerName(layer)) + "=" + std::to_string(mByLayer[layer]);
  }
  out += "\n";
  if (mTopNodes.empty()) {
    out += "  no owning nodes — the queue carried no items to attribute.\n";
  } else {
    out += "  largest owners (dbg_node: prims/distinct):\n";
    for (const QueueNodeShare &share : mTopNodes) {
      out += "    " + std::to_string(share.node) + ": " + std::to_string(share.prims) + "/" +
             std::to_string(share.distinct) + "\n";
    }
  }
  if (looksLikeRunaway()) {
    out += "  VERDICT: RUNAWAY — most of this queue is geometry a submit path already pushed this "
           "frame. Find the re-submitting walk in the backtrace below; raising RQ_MAX would only "
           "postpone this abort.\n";
  } else if (repeats() == 0) {
    out += "  VERDICT: CAPACITY — every prim in this queue is distinct geometry, so nothing was "
           "re-submitted. This frame genuinely contains more than RQ_MAX distinct prims, and the "
           "measurement RQ_MAX was sized from no longer covers it.\n";
  } else {
    out += "  VERDICT: UNDECIDED — repeats are present but are not the majority. Neither a runaway nor "
           "a clean capacity shortfall; read the owner split above before changing anything.\n";
  }
  return out;
}

void logRenderQueueAttribution(int frame, const RqItem *items, int count) {
  static const lucent::Channel channel{"rqattr"};
  if (!channel) {
    return;
  }
  const RenderQueueAttribution attribution(items, count);
  lucent::info("rqattr", "f{} queue attribution:\n{}", frame, attribution.text());
}

void logFlushedRenderQueue(int frame, const RqItem *items, int count, std::uint32_t seq) {
  // The y RANGE says WHICH FRAMEBUFFER this queue was drawn into: ys[] carries the guest's draw offset,
  // so a double-buffered guest's two buffers show up as two disjoint bands. `n=0` says the active queue
  // is genuinely empty, not that the instrument was silent. `reemit` is retained at zero for schema
  // compatibility: a consumed queue never reaches a flush, so a re-emit is now impossible by lifecycle.
  static const lucent::Channel flushChannel{"rqflush"};
  if (flushChannel) {
    int ylo = 1 << 30;
    int yhi = -(1 << 30);
    for (int i = 0; i < count; i++) {
      for (int v = 0; v < items[i].nv; v++) {
        ylo = std::min(ylo, items[i].ys[v]);
        yhi = std::max(yhi, items[i].ys[v]);
      }
    }
    lucent::debug(flushChannel, "n={} reemit={} seq={} y=[{}..{}]", count, 0, seq, count ? ylo : 0, count ? yhi : 0);
  }
  logRenderQueueAttribution(frame, items, count);
}

void abortOnFullRenderQueue(const RqItem *items, int count) {
  const RenderQueueAttribution report(items, count);
  lucent::error("rq",
                "\nFATAL: render queue full ({} items) — refusing to drop prims (fail-fast).\n{}  Backtrace:",
                RQ_MAX,
                report.text());
  void *frames[32];
  const int depth = backtrace(frames, 32);
  psxport::host::emitBacktrace(lucent::Level::Error, "rq", frames, depth, 1);
  abort();
}

} // namespace psxport::render

#include "fps60_sequence_runs.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace psxport::fps60 {
namespace {

// The vertices the presented picture is drawn from. An item carries rounded integer screen verts
// always and sub-pixel float ones only for the engine-owned 3D world path, so the extent reads
// whichever pair the rasterizer would read — otherwise a world run's box would disagree with the
// pixels it produced by up to a pixel on every side.
void growTo(ScreenExtent &extent, const RqItem &item, bool first) {
  int left = std::numeric_limits<int>::max();
  int top = std::numeric_limits<int>::max();
  int right = std::numeric_limits<int>::min();
  int bottom = std::numeric_limits<int>::min();
  const int vertices = std::clamp((int)item.nv, 0, 4);
  for (int v = 0; v < vertices; ++v) {
    const int x = item.has_xyf ? (int)std::floor(item.xsf[v]) : item.xs[v];
    const int y = item.has_xyf ? (int)std::floor(item.ysf[v]) : item.ys[v];
    left = std::min(left, x);
    top = std::min(top, y);
    right = std::max(right, x);
    bottom = std::max(bottom, y);
  }
  if (vertices == 0) {
    return;
  }
  // Half-open, so a one-pixel prim is one pixel wide rather than zero.
  const ScreenExtent here{left, top, right + 1, bottom + 1};
  if (first || extent.empty()) {
    extent = here;
    return;
  }
  extent.x0 = std::min(extent.x0, here.x0);
  extent.y0 = std::min(extent.y0, here.y0);
  extent.x1 = std::max(extent.x1, here.x1);
  extent.y1 = std::max(extent.y1, here.y1);
}

} // namespace

void groupSequenceRuns(std::span<const RqItem *const> items,
                       const std::function<bool(const RqItem &)> &owned,
                       std::vector<SequenceRun> &runs) {
  runs.clear();
  std::size_t i = 0;
  while (i < items.size()) {
    SequenceRun run{};
    run.layer = items[i]->layer;
    run.owned = owned(*items[i]);
    run.painterObject = items[i]->painter_object;
    run.dbgNode = items[i]->dbg_node;
    run.begin = i;
    std::size_t end = i + 1;
    while (end < items.size() && items[end]->layer == run.layer && items[end]->painter_object == run.painterObject &&
           items[end]->dbg_node == run.dbgNode && owned(*items[end]) == run.owned) {
      ++end;
    }
    run.end = end;
    for (std::size_t item = run.begin; item < run.end; ++item) {
      growTo(run.extent, *items[item], item == run.begin);
    }
    runs.push_back(run);
    i = end;
  }
}

} // namespace psxport::fps60

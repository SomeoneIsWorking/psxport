#include "fps60_sequence_runs.h"

namespace psxport::fps60 {

void groupSequenceRuns(std::span<const RqItem> items,
                       const std::function<bool(const RqItem &)> &owned,
                       std::vector<SequenceRun> &runs) {
  runs.clear();
  std::size_t i = 0;
  while (i < items.size()) {
    SequenceRun run{};
    run.layer = items[i].layer;
    run.owned = owned(items[i]);
    run.painterObject = items[i].painter_object;
    run.begin = i;
    std::size_t end = i + 1;
    while (end < items.size() && items[end].layer == run.layer && items[end].painter_object == run.painterObject &&
           owned(items[end]) == run.owned) {
      ++end;
    }
    run.end = end;
    runs.push_back(run);
    i = end;
  }
}

} // namespace psxport::fps60

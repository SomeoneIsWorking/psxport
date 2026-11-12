// write_rollback.h — the guest bytes a run overwrote and what they held, so the run can be undone.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

namespace psx {

// While armed, every store notes the bytes it is about to overwrite; `rollBack` writes them back, newest first,
// and closes the run. A run armed inside another nests: its rollBack puts back only what it wrote, and the outer
// run's still puts back everything. A run may keep host ranges: a store wholly inside one is not put back by that
// run's rollBack, so it reaches the run outside it (or the machine, when no run is left that does not keep it).
// The log keeps its capacity from run to run.
class WriteRollback {
public:
  [[nodiscard]] bool armed() const {
    return !runs_.empty();
  }
  // Which run is armed, so an owner keeping a value notes it once per run; each nested run has its own.
  [[nodiscard]] std::uint32_t run() const {
    return run_;
  }
  void arm() {
    if (!runs_.empty()) {
      settle(runs_.back());
    }
    runs_.push_back(Run{.entries = entries_.size(), .held = held_.size(), .keeps = keeps_.size(), .outer = run_});
    run_ = ++opened_;
  }
  // The innermost run leaves the `bytes` at `at` as its stores wrote them.
  void keep(const void *at, std::size_t bytes) {
    const auto begin = reinterpret_cast<std::uintptr_t>(at);
    keeps_.push_back(Range{.begin = begin, .end = begin + bytes});
    runs_.back().sorted = false;
  }
  // The innermost run keeps nothing after all: its rollBack puts back every byte it wrote.
  void keepNothing() {
    runs_.back().keeping = false;
  }
  // True when every open run keeps the store, so it outlives them all.
  bool note(std::uint8_t *at, std::uint32_t width) {
    const std::size_t held = held_.size();
    entries_.push_back(Entry{.at = at, .width = width, .held = held});
    held_.resize(held + width);
    std::memcpy(held_.data() + held, at, width);
    if (keeps_.empty()) {
      return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(at);
    for (std::size_t run = runs_.size(); run-- > 0;) {
      if (!keeps(run, begin, begin + width)) {
        return false;
      }
    }
    return true;
  }
  void rollBack() {
    const std::size_t inner = runs_.size() - 1;
    const Run run = runs_.back();
    const auto kept = [&](const Entry &entry) {
      const auto begin = reinterpret_cast<std::uintptr_t>(entry.at);
      return keeps(inner, begin, begin + entry.width);
    };
    for (std::size_t i = entries_.size(); i-- > run.entries;) {
      if (!kept(entries_[i])) {
        std::memcpy(entries_[i].at, held_.data() + entries_[i].held, entries_[i].width);
      }
    }
    if (inner == 0) {
      runs_.clear();
      entries_.clear();
      held_.clear();
      keeps_.clear();
      return;
    }
    // What this run kept is still the outer run's to put back.
    std::size_t entry = run.entries;
    std::size_t held = run.held;
    for (std::size_t i = run.entries; i < entries_.size(); ++i) {
      const Entry moved = entries_[i];
      if (!kept(moved)) {
        continue;
      }
      std::memmove(held_.data() + held, held_.data() + moved.held, moved.width);
      entries_[entry] = Entry{.at = moved.at, .width = moved.width, .held = held};
      held += moved.width;
      ++entry;
    }
    entries_.resize(entry);
    held_.resize(held);
    keeps_.resize(run.keeps);
    runs_.pop_back();
    run_ = run.outer;
  }

private:
  struct Entry {
    std::uint8_t *at = nullptr;
    std::uint32_t width = 0;
    std::size_t held = 0; // offset of the old bytes in held_
  };
  struct Range {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
  };
  struct Run {
    std::size_t entries = 0; // the log as the run found it
    std::size_t held = 0;
    std::size_t keeps = 0; // its kept ranges start here in keeps_
    std::uint32_t outer = 0;
    bool keeping = true;
    bool sorted = true; // its ranges ordered and merged
  };

  // The run's ranges ordered by start and merged; only the innermost run's can still be unsorted.
  void settle(Run &run) {
    if (run.sorted) {
      return;
    }
    const auto first = keeps_.begin() + static_cast<std::ptrdiff_t>(run.keeps);
    std::sort(first, keeps_.end(), [](const Range &a, const Range &b) {
      return a.begin < b.begin;
    });
    auto out = first;
    for (auto range = first; range != keeps_.end(); ++range) {
      if (out != first && range->begin <= std::prev(out)->end) {
        std::prev(out)->end = std::max(std::prev(out)->end, range->end);
      } else {
        *out++ = *range;
      }
    }
    keeps_.erase(out, keeps_.end());
    run.sorted = true;
  }
  [[nodiscard]] bool keeps(std::size_t index, std::uintptr_t begin, std::uintptr_t end) {
    Run &run = runs_[index];
    if (!run.keeping) {
      return false;
    }
    settle(run);
    const auto first = keeps_.begin() + static_cast<std::ptrdiff_t>(run.keeps);
    const auto last =
        index + 1 < runs_.size() ? keeps_.begin() + static_cast<std::ptrdiff_t>(runs_[index + 1].keeps) : keeps_.end();
    auto range = std::upper_bound(first, last, begin, [](std::uintptr_t at, const Range &r) {
      return at < r.begin;
    });
    return range != first && end <= (--range)->end;
  }

  std::vector<Entry> entries_;
  std::vector<std::uint8_t> held_;
  std::vector<Range> keeps_;
  std::vector<Run> runs_;
  std::uint32_t run_ = 0;
  std::uint32_t opened_ = 0;
};

} // namespace psx

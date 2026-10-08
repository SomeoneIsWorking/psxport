// producer_census.cpp — the per-run producer table and its feed from sealed frame records.
#include "producer_census.h"

#include "core.h"
#include "native_dispatch.h"
#include "ot_attr.h"

#include <lucent/log.h>

#include <algorithm>
#include <iterator>
#include <variant>

namespace psx::debug {

void ProducerCensus::noteRecord(const present::FrameRecord &record,
                                const Resolve &resolve,
                                const HasProducer &hasProducer) {
  fed_ = true;
  uses_.clear();
  for (const present::RecordEntry &entry : record.entries()) {
    const auto *primitive = std::get_if<present::DrawPrimitive>(&entry);
    if (primitive != nullptr && primitive->key) {
      uses_[*primitive->key]++;
    }
  }
  for (const present::RecordEntry &entry : record.entries()) {
    const auto *primitive = std::get_if<present::DrawPrimitive>(&entry);
    if (primitive == nullptr) {
      continue;
    }
    primitives_++;
    if (primitive->key) {
      keyed_++;
      Row &row = note(primitive->key->producer, true, hasProducer(primitive->key->producer));
      if (uses_[*primitive->key] > 1u) {
        duplicated_++;
        if (row.duplicated == 0u) {
          row.firstDuplicate = *primitive->key;
          row.firstDuplicateWriter = resolve(primitive->sourceAddress).submitter;
        }
        row.duplicated++;
      }
      continue;
    }
    const ProducerAttribution attribution = resolve(primitive->sourceAddress);
    if (attribution.submitter == 0) {
      unattributed_[static_cast<int>(attribution.why)]++;
      continue;
    }
    note(attribution.submitter, false, hasProducer(attribution.submitter));
  }
}

ProducerCensus::Row &ProducerCensus::note(std::uint32_t submitter, bool keyed, bool hasProducer) {
  const auto [slot, inserted] = index_.try_emplace(submitter, rows_.size());
  if (inserted) {
    rows_.push_back(Row{submitter});
  }
  Row &row = rows_[slot->second];
  row.hasProducer = hasProducer;
  row.primitives++;
  row.keyed += keyed ? 1u : 0u;
  return row;
}

const ProducerCensus::Row *ProducerCensus::find(std::uint32_t submitter) const {
  const auto found = index_.find(submitter);
  return found == index_.end() ? nullptr : &rows_[found->second];
}

void ProducerCensus::summary(const char *who) const {
  if (!fed_) {
    return;
  }
  lucent::info("producers",
               "{}: {} row(s); primitives {} = keyed {} + unkeyed attributed {} + no-source {} + span-miss {} + "
               "span-no-fn {}; duplicated keys {}",
               who,
               rows_.size(),
               primitives_,
               keyed_,
               primitives_ - keyed_ - unattributed_[0] - unattributed_[1] - unattributed_[2],
               unattributed_[0],
               unattributed_[1],
               unattributed_[2],
               duplicated_);
}

void ProducerCensus::report(const char *who) const {
  if (!fed_) {
    lucent::warn("producers", "{}: the producer census was never fed (no record sealed, or PSXPORT_PRODUCERS=0)", who);
    return;
  }
  summary(who);
  std::vector<const Row *> ranked;
  ranked.reserve(rows_.size());
  for (const Row &row : rows_) {
    ranked.push_back(&row);
  }
  std::stable_sort(ranked.begin(), ranked.end(), [](const Row *a, const Row *b) {
    return a->primitives - a->keyed > b->primitives - b->keyed;
  });
  const std::size_t shown = std::min<std::size_t>(ranked.size(), 16);
  for (std::size_t i = 0; i < shown; i++) {
    const Row &row = *ranked[i];
    lucent::info("producers",
                 "  0x{:08X} {} unkeyed {} keyed {}",
                 row.submitter,
                 row.hasProducer ? "producer   " : "no producer",
                 row.primitives - row.keyed,
                 row.keyed);
  }
  if (ranked.size() > shown) {
    lucent::info("producers", "  {} more row(s) not shown", ranked.size() - shown);
  }
  std::vector<const Row *> duplicated;
  std::copy_if(ranked.begin(), ranked.end(), std::back_inserter(duplicated), [](const Row *row) {
    return row->duplicated != 0u;
  });
  for (const Row *row : duplicated) {
    const present::RecordKey &key = row->firstDuplicate;
    lucent::info("producers",
                 "  0x{:08X} duplicated {} of keyed {}, first (0x{:08X}, 0x{:08X}, 0x{:X}, {}) written by 0x{:08X}",
                 row->submitter,
                 row->duplicated,
                 row->keyed,
                 key.producer,
                 key.object,
                 key.element,
                 key.part,
                 row->firstDuplicateWriter);
  }
}

void censusRecord(Core &core, const present::FrameRecord &record) {
  if (!g_producer_census_armed) {
    return;
  }
  const cpu::NativeDispatcher &dispatcher = core.nativeDispatcher();
  core.rsub.census.noteRecord(
      record,
      [&core](std::uint32_t packetAddress) {
        return core.rsub.otAttr.submitterOf(&core, packetAddress);
      },
      [&dispatcher](std::uint32_t submitter) {
        return dispatcher.isProducer(submitter);
      });
}

} // namespace psx::debug

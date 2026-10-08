// producer_census.h — the per-run producer table: primitives drawn per guest submitter, how many of
// them carried a producer key, and how many shared their key with another primitive of the same record
// (ambiguous, so never interpolated). Unkeyed rows ranked by primitive count are the producer work queue.
#pragma once

#include "frame_record.h"

#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

class Core;

namespace psx::debug {

// Why a primitive's submitter is unknown.
enum class Unattributed : std::uint8_t {
  NoSource = 0, // the packet was not read from guest RAM
  SpanMiss = 1, // no packet-store span covers its address
  SpanNoFn = 2, // a span covers it but names no function
};
inline constexpr int kUnattributedReasons = 3;

// A primitive's submitter, or 0 and the reason there is none.
struct ProducerAttribution {
  std::uint32_t submitter = 0;
  Unattributed why = Unattributed::NoSource;
};

class ProducerCensus {
public:
  struct Row {
    std::uint32_t submitter = 0; // guest function address, the override table's key space
    bool hasProducer = false;    // a producer override is registered at `submitter`
    std::uint64_t primitives = 0;
    std::uint64_t keyed = 0;
    std::uint64_t duplicated = 0; // keyed primitives whose key another primitive of its record also carries
    present::RecordKey firstDuplicate{};
    std::uint32_t firstDuplicateWriter = 0; // the guest function that stored that packet, 0 if unknown
  };
  using Resolve = std::function<ProducerAttribution(std::uint32_t packetAddress)>;
  using HasProducer = std::function<bool(std::uint32_t submitter)>;

  // Every primitive of a sealed record: a keyed one under its key's producer, an unkeyed one under
  // `resolve(sourceAddress)`.
  void noteRecord(const present::FrameRecord &record, const Resolve &resolve, const HasProducer &hasProducer);

  bool wasFed() const {
    return fed_;
  }
  std::uint64_t primsSeen() const {
    return primitives_;
  }
  std::uint64_t primsKeyed() const {
    return keyed_;
  }
  std::uint64_t primsDuplicated() const {
    return duplicated_;
  }
  std::uint64_t unattributed(Unattributed why) const {
    return unattributed_[static_cast<int>(why)];
  }
  std::span<const Row> rows() const {
    return rows_;
  }
  const Row *find(std::uint32_t submitter) const;

  // The one summary line; nothing when the census was never fed.
  void summary(const char *who) const;
  // The summary, the rows ranked by unkeyed primitives, then every row with duplicated keys.
  void report(const char *who) const;

private:
  Row &note(std::uint32_t submitter, bool keyed, bool hasProducer);

  std::vector<Row> rows_;
  std::unordered_map<std::uint32_t, std::size_t> index_;
  std::unordered_map<present::RecordKey, std::uint32_t, present::RecordKeyHash> uses_; // one record's key counts
  std::uint64_t primitives_ = 0;
  std::uint64_t keyed_ = 0;
  std::uint64_t duplicated_ = 0;
  std::uint64_t unattributed_[kUnattributedReasons] = {};
  bool fed_ = false;
};

// Feeds `core`'s census from `record` when the census is armed (PSXPORT_PRODUCERS).
void censusRecord(Core &core, const present::FrameRecord &record);

} // namespace psx::debug

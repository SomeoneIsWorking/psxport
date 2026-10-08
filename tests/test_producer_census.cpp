// test_producer_census.cpp — the per-run producer table fed from sealed frame records.
#include "producer_census.h"
#include "testutil.h"

namespace {

using psx::debug::ProducerAttribution;
using psx::debug::ProducerCensus;
using psx::debug::Unattributed;
using psx::present::DrawPrimitive;
using psx::present::FrameRecord;
using psx::present::RecordKey;
using psx::present::VramFill;

constexpr std::uint32_t kProducer = 0x8001F798u;
constexpr std::uint32_t kGuestSubmitter = 0x80030000u;

DrawPrimitive primitiveFrom(std::uint32_t packet, std::optional<RecordKey> key) {
  DrawPrimitive primitive;
  primitive.vertexCount = 3;
  primitive.sourceAddress = packet;
  primitive.key = key;
  return primitive;
}

// Packets 0x801000xx were written by kGuestSubmitter; 0x802000xx by nothing a span knows.
ProducerAttribution resolve(std::uint32_t packet) {
  if (packet == 0) {
    return {0, Unattributed::NoSource};
  }
  if ((packet & 0xFFFFFF00u) == 0x80100000u) {
    return {kGuestSubmitter, Unattributed::NoSource};
  }
  return {0, Unattributed::SpanMiss};
}

bool registered(std::uint32_t submitter) {
  return submitter == kProducer;
}

} // namespace

static void test_unfed_is_distinguishable_from_empty(void) {
  ProducerCensus census;
  CHECK(!census.wasFed());
  census.noteRecord(FrameRecord(0, true), resolve, registered);
  CHECK(census.wasFed());
  CHECK_EQ(census.primsSeen(), 0u);
  CHECK(census.rows().empty());
}

static void test_keyed_and_unkeyed_primitives_are_counted_per_submitter(void) {
  ProducerCensus census;
  FrameRecord record(0, true);
  record.append(primitiveFrom(0x80180004u, RecordKey{kProducer, 0x80150000u, 0, 0}));
  record.append(primitiveFrom(0x80180014u, RecordKey{kProducer, 0x80150040u, 0, 0}));
  record.append(primitiveFrom(0x80100004u, std::nullopt));
  record.append(primitiveFrom(0x80100024u, std::nullopt));
  record.append(primitiveFrom(0x80200004u, std::nullopt));
  record.append(primitiveFrom(0, std::nullopt));
  record.append(VramFill{});
  census.noteRecord(record, resolve, registered);
  census.noteRecord(record, resolve, registered);

  CHECK_EQ(census.primsSeen(), 12u);
  CHECK_EQ(census.primsKeyed(), 4u);
  CHECK_EQ(census.unattributed(Unattributed::SpanMiss), 2u);
  CHECK_EQ(census.unattributed(Unattributed::NoSource), 2u);
  CHECK_EQ(census.rows().size(), 2u);
  const ProducerCensus::Row *producer = census.find(kProducer);
  CHECK(producer != nullptr && producer->hasProducer);
  CHECK(producer != nullptr && producer->primitives == 4u && producer->keyed == 4u);
  const ProducerCensus::Row *guest = census.find(kGuestSubmitter);
  CHECK(guest != nullptr && !guest->hasProducer);
  CHECK(guest != nullptr && guest->primitives == 4u && guest->keyed == 0u);
}

static void test_has_producer_follows_the_registrations(void) {
  ProducerCensus census;
  FrameRecord record(0, true);
  record.append(primitiveFrom(0x80100004u, std::nullopt));
  census.noteRecord(record, resolve, registered);
  CHECK(!census.find(kGuestSubmitter)->hasProducer);
  census.noteRecord(record, resolve, [](std::uint32_t submitter) {
    return submitter == kGuestSubmitter;
  });
  CHECK(census.find(kGuestSubmitter)->hasProducer);
  CHECK_EQ(census.find(kGuestSubmitter)->keyed, 0u);
}

static void test_a_key_shared_within_a_record_is_counted_as_duplicated(void) {
  ProducerCensus census;
  const RecordKey shared{kProducer, 0x80150000u, 0x10003u, 0};
  FrameRecord record(0, true);
  record.append(primitiveFrom(0x80100004u, shared));
  record.append(primitiveFrom(0x80180034u, shared));
  record.append(primitiveFrom(0x80180064u, RecordKey{kProducer, 0x80150000u, 0x20003u, 0}));
  record.append(primitiveFrom(0x80100004u, std::nullopt));
  census.noteRecord(record, resolve, registered);

  // The same key in the next record is a pairing, not a duplicate.
  FrameRecord next(1, true);
  next.append(primitiveFrom(0x80180004u, shared));
  census.noteRecord(next, resolve, registered);

  CHECK_EQ(census.primsKeyed(), 4u);
  CHECK_EQ(census.primsDuplicated(), 2u);
  const ProducerCensus::Row *producer = census.find(kProducer);
  CHECK(producer != nullptr && producer->duplicated == 2u);
  CHECK(producer != nullptr && producer->firstDuplicate == shared);
  CHECK(producer != nullptr && producer->firstDuplicateWriter == kGuestSubmitter);
  CHECK_EQ(census.find(kGuestSubmitter)->duplicated, 0u);
}

int main(void) {
  RUN(unfed_is_distinguishable_from_empty);
  RUN(a_key_shared_within_a_record_is_counted_as_duplicated);
  RUN(keyed_and_unkeyed_primitives_are_counted_per_submitter);
  RUN(has_producer_follows_the_registrations);
  return pt_summary();
}

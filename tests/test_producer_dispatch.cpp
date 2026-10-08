// test_producer_dispatch.cpp — a producer override's packets carry (P, obj, k) into the frame record.
#include "testutil.h"

#include "core.h"
#include "game.h"
#include "game_runtime.h"
#include "native_dispatch.h"
#include "ordering_table.h"
#include "producer_census.h"

#include <memory>
#include <variant>
#include <vector>

namespace {

constexpr std::uint32_t kEntry = 0x00010000u;
constexpr std::uint32_t kReturn = 0x00030000u;
constexpr std::uint32_t kObject = 0x80150040u;
constexpr std::uint32_t kOt = 0x80110000u;
constexpr std::uint32_t kKeyedOne = 0x80100000u;
constexpr std::uint32_t kKeyedTwo = 0x80100020u;
constexpr std::uint32_t kGuestPacket = 0x80100060u;
constexpr std::uint32_t kChainEnd = 0x00FFFFFFu;

class Runtime final : public GameRuntime {
public:
  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void bootInit(Core &) override {}
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }
  void registerOverrides(Game &) override {}
};

std::uint32_t link(std::uint32_t words, std::uint32_t next) {
  return (words << 24) | (next & 0x00FFFFFFu);
}

// A flat triangle at `address + 4`; the header is written first, as libgpu's addPrim does.
void writeTriangles(Core &core, std::uint32_t address, std::uint32_t next, int count) {
  core.mem_w32(address, link(4u * static_cast<std::uint32_t>(count), next));
  for (int t = 0; t < count; t++) {
    const std::uint32_t at = address + 4u + 16u * static_cast<std::uint32_t>(t);
    core.mem_w32(at, 0x20808080u);
    core.mem_w32(at + 4u, 0x00000000u);
    core.mem_w32(at + 8u, 0x00000010u);
    core.mem_w32(at + 12u, 0x00100000u);
  }
}

std::size_t g_depthInside = 0;
std::uint32_t g_objectInside = 0;

void drawObject(Core *core) {
  g_depthInside = core->emission.depth();
  g_objectInside = core->r[5];
  writeTriangles(*core, kKeyedOne, kKeyedTwo, 1);
  writeTriangles(*core, kKeyedTwo, kGuestPacket, 2);
}

struct Fixture {
  Runtime runtime;
  std::unique_ptr<Game> game;
  bool invoked = false;

  Fixture() {
    psxport_install_game(runtime);
    game = std::make_unique<Game>();
    Core &core = game->core;
    core.imageCatalog().activate("producer-test", {kEntry, kEntry + 8u}, 0x50524F44u);
    core.mem_w32(kEntry, 0x03E00008u); // jr $ra
    core.mem_w32(kEntry + 4u, 0);
    core.mem_w32(kReturn, 0x03E00008u);
    core.mem_w32(kReturn + 4u, 0);
    psx::cpu::installNativeOverride(
        core, kEntry, "test producer", &drawObject, psx::present::Producer{psx::present::Arg::A1});
    core.gpuDevice.gp1(0, 0);
    core.gpuDevice.sealRecord();
  }

  psx::cpu::NativeKey key() {
    return psx::cpu::NativeKey{*game->core.imageCatalog().resolve(kEntry), kEntry};
  }

  // One logic frame: the producer runs, guest code stores its own packet, the OT is walked.
  psx::present::FrameRecord frame() {
    Core &core = game->core;
    core.r[31] = kReturn;
    core.r[5] = kObject;
    invoked = core.nativeDispatcher().invoke(key()).has_value();
    writeTriangles(core, kGuestPacket, kChainEnd, 1);
    core.mem_w32(kOt, link(0, kKeyedOne));
    psx::gpu::submitOrderingTable(core, core.gpuDevice, kOt);
    return core.gpuDevice.sealRecord();
  }
};

std::vector<psx::present::DrawPrimitive> primitives(const psx::present::FrameRecord &record) {
  std::vector<psx::present::DrawPrimitive> out;
  for (const psx::present::RecordEntry &entry : record.entries()) {
    if (const auto *primitive = std::get_if<psx::present::DrawPrimitive>(&entry)) {
      out.push_back(*primitive);
    }
  }
  return out;
}

} // namespace

static void test_the_dispatcher_scopes_the_override_with_its_object(void) {
  Fixture fixture;
  CHECK(fixture.game->core.nativeDispatcher().isProducer(kEntry));
  const psx::present::FrameRecord record = fixture.frame();
  CHECK(fixture.invoked);
  CHECK_EQ(g_depthInside, 1u);
  CHECK_EQ(g_objectInside, kObject);
  CHECK_EQ(fixture.game->core.emission.depth(), 0u);

  const std::vector<psx::present::DrawPrimitive> drawn = primitives(record);
  CHECK_EQ(drawn.size(), 4u);
  CHECK(drawn[0].key.has_value() && drawn[0].key->producer == kEntry && drawn[0].key->object == kObject);
  CHECK(drawn[0].key.has_value() && drawn[0].key->element == 0u && drawn[0].key->part == 0u);
  // part counts within one packet; the second packet repeats the first's key, which pairing rejects.
  CHECK(drawn[1].key.has_value() && drawn[1].key->element == 0u && drawn[1].key->part == 0u);
  CHECK(drawn[2].key.has_value() && drawn[2].key->element == 0u && drawn[2].key->part == 1u);
  CHECK(!drawn[3].key.has_value());
}

// A packet keeps its key across the seal until guest code outside the producer rewrites it.
static void test_keys_follow_the_packet_writer_across_frames(void) {
  Fixture fixture;
  Core &core = fixture.game->core;
  const psx::present::FrameRecord first = fixture.frame();
  writeTriangles(core, kKeyedOne, kKeyedTwo, 1);
  core.mem_w32(kOt, link(0, kKeyedOne));
  psx::gpu::submitOrderingTable(core, core.gpuDevice, kOt);
  const std::vector<psx::present::DrawPrimitive> walked = primitives(core.gpuDevice.sealRecord());
  CHECK_EQ(walked.size(), 4u);
  CHECK(!walked[0].key.has_value());
  CHECK(walked[1].key.has_value() && walked[1].key->object == kObject);
  CHECK(walked[2].key.has_value() && walked[2].key->object == kObject);
  CHECK(!walked[3].key.has_value());
  // The next frame's call keys the same packets with the same keys again.
  const std::vector<psx::present::DrawPrimitive> again = primitives(fixture.frame());
  const std::vector<psx::present::DrawPrimitive> before = primitives(first);
  CHECK_EQ(again.size(), before.size());
  for (std::size_t i = 0; i < again.size() && i < before.size(); i++) {
    CHECK(again[i].key.has_value() == before[i].key.has_value());
    if (again[i].key && before[i].key) {
      CHECK(again[i].key->object == before[i].key->object && again[i].key->element == before[i].key->element);
      // Each call is a new scope, naming the state it saved.
      CHECK(again[i].key->serial != before[i].key->serial);
    }
  }
}

// Bucket heads of a named table tag what follows them; the record keeps where each bucket began.
static void test_the_walk_tags_each_primitive_with_its_named_bucket(void) {
  Fixture fixture;
  Core &core = fixture.game->core;
  core.otTables.name(3, kOt, 3, 4, psx::gpu::OtWalk::LowToHigh);
  core.mem_w32(kOt, link(0, kKeyedOne));
  writeTriangles(core, kKeyedOne, kOt + 4u, 1);
  core.mem_w32(kOt + 4u, link(0, kOt + 8u));
  core.mem_w32(kOt + 8u, link(0, kGuestPacket));
  writeTriangles(core, kGuestPacket, kChainEnd, 1);
  psx::gpu::submitOrderingTable(core, core.gpuDevice, kOt);
  const psx::present::FrameRecord record = core.gpuDevice.sealRecord();
  const std::vector<psx::present::DrawPrimitive> drawn = primitives(record);
  CHECK_EQ(drawn.size(), 2u);
  CHECK(drawn[0].slot == (psx::present::OtSlot{3, 0}));
  CHECK(drawn[1].slot == (psx::present::OtSlot{3, 2}));
  const std::vector<psx::present::SlotStart> expected{{{3, 0}, 0, false}, {{3, 1}, 1, false}, {{3, 2}, 1, false}};
  CHECK(std::vector<psx::present::SlotStart>(record.slotStarts().begin(), record.slotStarts().end()) == expected);

  core.otTables.clear();
  psx::gpu::submitOrderingTable(core, core.gpuDevice, kOt);
  for (const psx::present::DrawPrimitive &primitive : primitives(core.gpuDevice.sealRecord())) {
    CHECK(!primitive.slot.has_value());
  }
}

// A title that flattens its buckets into one chain assigns each packet its bucket before the walk.
static void test_assigned_buckets_tag_a_flattened_chain_for_one_walk(void) {
  Fixture fixture;
  Core &core = fixture.game->core;
  core.otTables.name(4, 0x80120000u, 512, 8, psx::gpu::OtWalk::HighToLow);
  writeTriangles(core, kKeyedOne, kGuestPacket, 1);
  writeTriangles(core, kGuestPacket, kChainEnd, 1);
  core.otTables.assign(kKeyedOne, {4, 7});
  core.otTables.assign(kGuestPacket, {4, 2});
  psx::gpu::submitOrderingTable(core, core.gpuDevice, kKeyedOne);
  const psx::present::FrameRecord record = core.gpuDevice.sealRecord();
  const std::vector<psx::present::DrawPrimitive> drawn = primitives(record);
  CHECK_EQ(drawn.size(), 2u);
  CHECK(drawn[0].slot == (psx::present::OtSlot{4, 7}));
  CHECK(drawn[1].slot == (psx::present::OtSlot{4, 2}));
  const std::vector<psx::present::SlotStart> expected{{{4, 7}, 0, true}, {{4, 2}, 1, true}};
  CHECK(std::vector<psx::present::SlotStart>(record.slotStarts().begin(), record.slotStarts().end()) == expected);

  psx::gpu::submitOrderingTable(core, core.gpuDevice, kKeyedOne);
  for (const psx::present::DrawPrimitive &primitive : primitives(core.gpuDevice.sealRecord())) {
    CHECK(!primitive.slot.has_value());
  }
}

static void test_the_census_counts_keyed_primitives_under_the_producer(void) {
  Fixture fixture;
  Core &core = fixture.game->core;
  psx::debug::censusRecord(core, fixture.frame());
  const psx::debug::ProducerCensus::Row *row = core.rsub.census.find(kEntry);
  CHECK(row != nullptr && row->hasProducer && row->keyed == 3u && row->primitives == 3u);
  CHECK_EQ(core.rsub.census.primsSeen(), 4u);
}

int main(void) {
  RUN(the_dispatcher_scopes_the_override_with_its_object);
  RUN(keys_follow_the_packet_writer_across_frames);
  RUN(the_walk_tags_each_primitive_with_its_named_bucket);
  RUN(assigned_buckets_tag_a_flattened_chain_for_one_walk);
  RUN(the_census_counts_keyed_primitives_under_the_producer);
  return pt_summary();
}

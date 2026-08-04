// test_overlay_reloc — the live placement registry for RELOCATABLE overlay modules.
//
// WHAT IT GUARDS. A game whose code modules are loaded and relocated at runtime (Spider-Man's 30
// CD.WAD modules) has no fixed address to route dispatch by: the game's own allocator picks each
// module's base, it differs per module and per playthrough, and SEVERAL ARE RESIDENT AT ONCE. Those
// modules are recompiled base-relative against a shared LINK base, so every one of them reports the
// SAME `base`/`end` in the generated overlay table — routing on that static range is not merely
// imprecise, it is ambiguous by construction.
//
// The bug this replaces (spider1 docs/issues/0001): all 30 modules were pinned to one guest address
// on the premise that only one was ever resident. A level load puts L5A5LSC, LIZMAN and VENOM live
// together; the third overwrote the first two, and the guest's next dispatch through LIZMAN's own
// still-live method table landed inside VENOM's epilogue — `recomp-MISS 0x800C6684`, before the port
// ever reached a screen.
//
// SO THE CASES BELOW ARE THE ONES THAT DECIDE IT: three modules that share a link range, live at
// three different bases at the same time, each answering for its own addresses; an address inside
// the shared LINK range that belongs to NOBODY while no module is placed there; and eviction.
//
// A negative here carries its denominator: each case counts the addresses it probed, so "resolved
// correctly" is never a claim about a lookup that was not performed.
//
// HERMETIC: no disc, no GPU, no Game. A synthetic RecompRegistry is installed and a bare Core is
// constructed; the registry functions under test touch nothing else.
//
// WHAT IT CANNOT COVER: the emitted code's own delta arithmetic (that is the recompiler's gate —
// emit.py's HI16-consumer check plus the game-side relocation-model checker), and the overlapping-
// placement fail-fast, which aborts the process by design and so cannot be exercised in-process.
#include "testutil.h"

#include "core.h"
#include "overlay_router.h"
#include "recomp_iface.h"

#include <memory>

namespace {

// Three modules recompiled at ONE link base — exactly the shape the generated table has for a game
// with relocatable modules, and the shape that makes static-range routing ambiguous.
constexpr uint32_t kLinkBase = 0x800C65ECu;
constexpr uint32_t kSizeA = 0x800u, kSizeB = 0x8800u, kSizeC = 0x9800u;

void disp_stub(Core*, uint32_t) {}
int idx_stub(uint32_t) { return -1; }

const RecOverlay kOverlays[] = {
    {kLinkBase, kLinkBase + kSizeA, "L5A5LSC", disp_stub, idx_stub, nullptr, 0, 1},
    {kLinkBase, kLinkBase + kSizeB, "LIZMAN", disp_stub, idx_stub, nullptr, 0, 1},
    {kLinkBase, kLinkBase + kSizeC, "VENOM", disp_stub, idx_stub, nullptr, 0, 1},
    // A FIXED-base overlay alongside them: the registry must ignore it entirely, or a game that has
    // both kinds (the framework supports both) would see its pinned overlays answer for live ranges.
    {0x80106228u, 0x80106228u + 0x1000u, "PINNED", disp_stub, idx_stub, nullptr, 0, 0},
};

const RecompRegistry kRegistry = {
    nullptr, nullptr, kOverlays, (int)(sizeof kOverlays / sizeof kOverlays[0]),
    nullptr, nullptr, nullptr, nullptr,
};

// The three live bases measured from a real boot of the port (scratch/logs/boot_after.log): the
// game's allocator placed the three co-resident modules here, and they are nowhere near each other
// or near the link base.
constexpr uint32_t kLiveA = 0x8014A6D0u;   // L5A5LSC
constexpr uint32_t kLiveB = 0x801BDA30u;   // LIZMAN
constexpr uint32_t kLiveC = 0x801C6238u;   // VENOM

std::unique_ptr<Core> fresh_core() {
  psxport_install_recomp(&kRegistry);
  return std::make_unique<Core>();
}

}  // namespace

// THE CASE THE PINNED DESIGN GOT WRONG: three modules live at once, each owning its own addresses.
static void test_three_coresident_modules_route_separately(void) {
  auto c = fresh_core();
  CHECK_EQ(overlay_place(c.get(), "L5A5LSC", kLiveA, kSizeA), 0);
  CHECK_EQ(overlay_place(c.get(), "LIZMAN", kLiveB, kSizeB), 1);
  CHECK_EQ(overlay_place(c.get(), "VENOM", kLiveC, kSizeC), 2);

  // Probe each module at its first, middle and last word — 9 lookups, none of which may answer with
  // a sibling that shares the link range.
  int probed = 0;
  const uint32_t live[3] = {kLiveA, kLiveB, kLiveC};
  const uint32_t size[3] = {kSizeA, kSizeB, kSizeC};
  for (int i = 0; i < 3; ++i)
    for (uint32_t off : {0u, size[i] / 2, size[i] - 4}) {
      CHECK_EQ(overlay_live_index(c.get(), live[i] + off), i);
      ++probed;
    }
  CHECK_EQ(probed, 9);

  // The delta each module's recompiled code adds to every address it composes.
  CHECK_EQ(c->ovDelta[0], (int32_t)(kLiveA - kLinkBase));
  CHECK_EQ(c->ovDelta[1], (int32_t)(kLiveB - kLinkBase));
  CHECK_EQ(c->ovDelta[2], (int32_t)(kLiveC - kLinkBase));

  // 0x800C6684 — the exact address the pinned port died dispatching. It lies inside the shared LINK
  // range, and under the live registry it belongs to NO module, because no module lives there.
  CHECK_EQ(overlay_live_index(c.get(), 0x800C6684u), -1);
}

// An address just past a module's end is not that module's. Off-by-one here would hand a dispatch
// to a neighbour whose code does not contain it.
static void test_live_range_is_half_open(void) {
  auto c = fresh_core();
  CHECK_EQ(overlay_place(c.get(), "LIZMAN", kLiveB, kSizeB), 1);
  CHECK_EQ(overlay_live_index(c.get(), kLiveB), 1);
  CHECK_EQ(overlay_live_index(c.get(), kLiveB + kSizeB - 4), 1);
  CHECK_EQ(overlay_live_index(c.get(), kLiveB + kSizeB), -1);
  CHECK_EQ(overlay_live_index(c.get(), kLiveB - 4), -1);
}

// Eviction: after the guest frees a module body, nothing may dispatch into it any more.
static void test_evict_releases_the_range(void) {
  auto c = fresh_core();
  CHECK_EQ(overlay_place(c.get(), "VENOM", kLiveC, kSizeC), 2);
  CHECK_EQ(overlay_live_index(c.get(), kLiveC + 0x100), 2);
  CHECK_EQ(overlay_evict_at(c.get(), kLiveC), 2);
  CHECK_EQ(overlay_live_index(c.get(), kLiveC + 0x100), -1);
  CHECK_EQ(c->ovDelta[2], 0);
  // A free of ordinary heap memory is not an eviction — the loader calls this on EVERY free.
  CHECK_EQ(overlay_evict_at(c.get(), 0x80123456u), -1);
}

// The same module reloaded lands somewhere else. Its delta must follow, not stick to the first load.
static void test_reload_moves_the_module(void) {
  auto c = fresh_core();
  CHECK_EQ(overlay_place(c.get(), "LIZMAN", kLiveB, kSizeB), 1);
  CHECK_EQ(overlay_evict_at(c.get(), kLiveB), 1);
  const uint32_t elsewhere = 0x80170000u;
  CHECK_EQ(overlay_place(c.get(), "LIZMAN", elsewhere, kSizeB), 1);
  CHECK_EQ(overlay_live_index(c.get(), elsewhere + 0x40), 1);
  CHECK_EQ(overlay_live_index(c.get(), kLiveB + 0x40), -1);
  CHECK_EQ(c->ovDelta[1], (int32_t)(elsewhere - kLinkBase));
}

// Per-Core, not global: two Cores are two machines, and a placement in one must not route in the
// other. (The SBS differential harness runs two.)
static void test_placement_is_per_core(void) {
  auto a = fresh_core();
  auto b = std::make_unique<Core>();
  CHECK_EQ(overlay_place(a.get(), "VENOM", kLiveC, kSizeC), 2);
  CHECK_EQ(overlay_live_index(a.get(), kLiveC + 8), 2);
  CHECK_EQ(overlay_live_index(b.get(), kLiveC + 8), -1);
  CHECK_EQ(b->ovDelta[2], 0);
}

// A fixed-base overlay is routed by its static signature/range, never by this registry, and a name
// that no module carries must be refused rather than silently placed.
static void test_registry_ignores_fixed_base_and_unknown_names(void) {
  auto c = fresh_core();
  CHECK_EQ(overlay_place(c.get(), "PINNED", 0x80106228u, 0x1000u), -1);
  CHECK_EQ(overlay_place(c.get(), "NOSUCHMODULE", 0x80140000u, 0x100u), -1);
  CHECK_EQ(overlay_live_index(c.get(), 0x80106228u + 0x10), -1);
}

int main(void) {
  RUN(three_coresident_modules_route_separately);
  RUN(live_range_is_half_open);
  RUN(evict_releases_the_range);
  RUN(reload_moves_the_module);
  RUN(placement_is_per_core);
  RUN(registry_ignores_fixed_base_and_unknown_names);
  return pt_summary();
}

// test_cd_hwcd_event_subscriber.cpp — a CD-ROM completion CANNOT be delivered as a BIOS hardware
// event to a guest that never opened one, and the number that looks like a fix is the one that
// cannot work.
//
// WHAT THIS PINS, AND WHY IT IS NOT THE OBVIOUS TEST. `Hle::deliverEvent` (runtime/psx/hle.cpp)
// marks a slot only when `ev[i].open && ev[i].enabled && ev[i].ev_class == evClass` and
// `(ev[i].spec & spec)`. A slot's `ev_class` is set in exactly one place — the B0:0x08 OpenEvent arm
// of `Hle::dispatchBios` — so "can this class be delivered to" is decided entirely by what the guest
// opened, and NOT by which spec a caller picks.
//
// The memory-card owner is the framework's own precedent for delivering a device completion as a
// PAIR of BIOS hardware classes (runtime/psx/memcard.cpp:405-406):
//
//     c->game->hle.deliverEvent(0xF4000001u, 0x0004u); // SwCARD I/O end
//     c->game->hle.deliverEvent(0xF0000011u, 0x0004u); // HwCARD BIOS-level completion
//
// That pair works for the titles that use it because those titles OPEN those classes. The CD
// equivalent class is HwCD = 0xF0000003, and Spider-Man 1 (SLUS_008.75) does not.
//
// MEASURED, not inferred. Over a 2,324-presented-frame run of the real disc, the product's own `ev`
// channel logged 9 `OpenEvent` calls across 3 classes — SwCARD 0xF4000001 x4, HwCARD 0xF0000011 x4,
// HwSPU 0xF0000009 x1 — and 0 with class 0xF0000003. Independently, an exhaustive scan of the whole
// 747,520-byte text segment finds exactly 9 `lui $r,0xF000` sites; constant propagation puts
// 0xF0000003 in a register at only 3 of them, and all 3 are the bodies CdInit installs into the libcd
// callback slots (0x8008A240 / 0x8008A268 / 0x8008A290) — each a DeliverEvent (B0:0x07) CALLER, not an
// OpenEvent (B0:0x08) call.
//
// The consequence is the reason this file exists: adding `deliverEvent(0xF0000003, spec)` to a CD
// path is a GUARANTEED NO-OP for that title, for EVERY spec, and a no-op that is announced as a fix
// is worse than the missing feature because it closes the frontier on a lie.
//
// THE POSITIVE CONTROL IS THE POINT. A test that only ever asserts "HwCD did not fire" passes for
// any reason at all, including a broken `deliverEvent` or a broken `OpenEvent`. So the SAME guest
// event table, the SAME delivery entry point, and the SAME matching rule are used to show that
// 0xF0000011 spec 0x0004 — the exact call memcard.cpp:406 makes — DOES fire. The card half and the CD
// half differ only in whether the class was opened. If a future change made HwCD fire without any
// OpenEvent, or stopped the card from firing, this file goes red in both directions.
//
// The guest registration below is the MEASURED SLUS_008.75 table, transcribed, not a convenient
// fixture. `kGuestEventTable` is the run's 9 opens; the class 0xF0000003 rows are deliberately absent
// because the run measured them absent.
//
// HERMETIC: a Game is constructed in-process. No disc, no window, no GPU, no wall clock.
#include "../runtime/psx/game.h"
#include "../runtime/psx/hle.h"
#include "testutil.h"

enum { R_A3 = 7 };

// The BIOS hardware event classes, as psxport's own memcard.cpp names them in the delivery comments.
static constexpr uint32_t kSwCard = 0xF4000001u;
static constexpr uint32_t kHwCard = 0xF0000011u;
static constexpr uint32_t kHwSpu = 0xF0000009u;
static constexpr uint32_t kHwCd = 0xF0000003u; // the class the CD-ROM completion would use

// SLUS_008.75's CdInit installs three libcd callback bodies into the callback slots, and each one's
// ENTIRE body is `DeliverEvent(0xF0000003, spec)`:
//     0x8008A238  a0 = 0xF0000003 ; jal 0x8008F9D0 (B0:0x07) ; a1 = 0x20
//     0x8008A260  a0 = 0xF0000003 ; jal 0x8008F9D0 (B0:0x07) ; a1 = 0x40
//     0x8008A288  a0 = 0xF0000003 ; jal 0x8008F9D0 (B0:0x07) ; a1 = 0x40
// 0x20 and 0x40 are therefore the guest's OWN HwCD specs, read from its bytes rather than chosen.
static constexpr uint32_t kHwCdSpecSync = 0x20u;
static constexpr uint32_t kHwCdSpecReady = 0x40u;

struct GuestEventOpen {
  uint32_t evClass;
  uint32_t spec;
};

// The MEASURED SLUS_008.75 registration: 9 OpenEvent calls, 3 classes, and no 0xF0000003.
static constexpr GuestEventOpen kGuestEventTable[] = {
    {kSwCard, 0x00000004u},
    {kSwCard, 0x00008000u},
    {kSwCard, 0x00000100u},
    {kSwCard, 0x00002000u},
    {kHwCard, 0x00000004u},
    {kHwCard, 0x00008000u},
    {kHwCard, 0x00000100u},
    {kHwCard, 0x00002000u},
    {kHwSpu, 0x00000020u},
};

// A FRESH Game per case, and that is not tidiness. `Hle::ev` is 16 slots on the Game, and these
// cases each register the guest's 9 opens; sharing one instance across four cases fills the table
// partway through and every later OpenEvent then returns 0xFFFFFFFF, which reads as "the guest
// cannot open this class" for a reason that has nothing to do with the class. That is exactly the
// capacity artifact the third case exists to rule out, so the fixture must not manufacture it.
static Game *gam() {
  return new Game();
}

static uint32_t bios_b0(Game *g, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
  Core *c = &g->core;
  c->r[4] = a0;
  c->r[5] = a1;
  c->r[6] = a2;
  c->r[2] = 0xDEADBEEFu; // so "the handler never wrote v0" cannot read as a plausible answer
  return g->hle.dispatchBios('B', fn) ? c->r[2] : 0xDEADBEEFu;
}

// Open + enable exactly as the guest does. EvMdNOINTR (0x2000) marks the slot and runs nothing, so
// this needs no guest code and measures only the class/spec matching rule.
static uint32_t open_event(Game *g, uint32_t cls, uint32_t spec) {
  g->core.r[R_A3] = 0;
  const uint32_t handle = bios_b0(g, 0x08, cls, spec, 0x2000u);
  bios_b0(g, 0x0C, handle, 0, 0); // EnableEvent — deliverEvent ignores a disabled slot
  return handle;
}

static bool test_event(Game *g, uint32_t handle) {
  return bios_b0(g, 0x0B, handle, 0, 0) != 0;
}

static void register_guest_table(Game *g) {
  for (const GuestEventOpen &open : kGuestEventTable) {
    open_event(g, open.evClass, open.spec);
  }
}

// Clear every `fired` flag by testing every open handle, so each case starts from a known state and
// "not fired" means "not delivered now" rather than "left over from the previous case".
static void drain_all(Game *g, const uint32_t *handles, int count) {
  for (int i = 0; i < count; i++) {
    (void)test_event(g, handles[i]);
  }
}

// THE POSITIVE CONTROL: the exact delivery memcard.cpp:405-406 performs. If this ever stops firing,
// the instrument is broken and the negative cases below mean nothing.
static void test_card_pair_delivery_fires_the_opened_slot(void) {
  Game *g = gam();
  register_guest_table(g);
  const uint32_t h = open_event(g, kHwCard, 0x0004u);
  drain_all(g, &h, 1);

  g->hle.deliverEvent(kSwCard, 0x0004u); // memcard.cpp:405
  g->hle.deliverEvent(kHwCard, 0x0004u); // memcard.cpp:406

  CHECK(test_event(g, h));
}

// The CD half: a delivery must reach NO slot, so this watches the WHOLE open table rather than one
// nominated handle.
//
// WHY TABLE-WIDE, and it is not a stylistic choice. The first version watched a single HwCARD
// spec-0x0004 handle and asserted it stayed unfired. A scratch discriminator that PLANTED one
// HwCD spec-0x40 open event — the one thing the measured table lacks — left that case GREEN, because
// delivering HwCD marks the planted HwCD slot and never touches the HwCARD handle being watched. A
// negative case that cannot see the failure it exists to catch is not a negative case, and it would
// have shipped as one. Asking "did ANY open slot fire" is the question the finding actually raises.
static int count_fired_slots(Game *g, const uint32_t *handles, int count) {
  int fired = 0;
  for (int i = 0; i < count; i++) {
    if (test_event(g, handles[i])) {
      fired++;
    }
  }
  return fired;
}

static void test_hwcd_cannot_fire_without_an_opened_slot(void) {
  Game *g = gam();
  uint32_t handles[sizeof(kGuestEventTable) / sizeof(kGuestEventTable[0])];
  for (size_t i = 0; i < sizeof(kGuestEventTable) / sizeof(kGuestEventTable[0]); i++) {
    handles[i] = open_event(g, kGuestEventTable[i].evClass, kGuestEventTable[i].spec);
  }
  const int handleCount = (int)(sizeof(handles) / sizeof(handles[0]));
  CHECK_EQ(count_fired_slots(g, handles, handleCount), 0); // starts clean

  // The guest's own two HwCD specs, read from its CdInit callback bodies, then every other spec a
  // caller might plausibly reach for. All of them must leave the entire table unfired. The count is
  // stated so an absent tail cannot read as "there were no specs to try".
  const uint32_t specs[] = {kHwCdSpecSync, kHwCdSpecReady, 0x0004u, 0x8000u, 0xFFFFFFFFu};
  const int specCount = (int)(sizeof(specs) / sizeof(specs[0]));
  int inert = 0;
  for (int i = 0; i < specCount; i++) {
    g->hle.deliverEvent(kHwCd, specs[i]);
    if (count_fired_slots(g, handles, handleCount) == 0) {
      inert++; // this spec marked nothing, out of the 9 open slots
    }
  }
  CHECK_EQ(inert, specCount); // all 5 of 5 tried specs marked 0 of 9 slots
}

// WHY the CD half above is inert, stated as a fact about the table rather than about the delivery: a
// HwCD slot is absent because the guest never opened one, and the framework is not going to invent
// it. This is the DENOMINATOR the negative case needs — 9 opens scanned, 0 of class 0xF0000003.
static void test_the_guest_event_table_has_no_hwcd_row(void) {
  int scanned = 0;
  int hwcd = 0;
  for (const GuestEventOpen &open : kGuestEventTable) {
    scanned++;
    if (open.evClass == kHwCd) {
      hwcd++;
    }
  }
  CHECK_EQ(scanned, 9); // the measured count, so a truncated table cannot read as an absence
  CHECK_EQ(hwcd, 0);
  // And the absence is not a CAPACITY artifact: after the measured 9 opens, a tenth still succeeds.
  // Without this, "no HwCD row" and "the table was full" are the same observation, and the first is
  // the only one that means anything.
  Game *g = gam();
  register_guest_table(g);
  const uint32_t spare = open_event(g, kHwCd, kHwCdSpecReady);
  // 0xFFFFFFFF is Hle::dispatchBios's own "table full" answer, so this asks the framework rather than
  // duplicating its capacity constant.
  CHECK(spare != 0xFFFFFFFFu);
}

// A DISCRIMINATOR for the other direction: HwCD delivery is not merely "late", it is structurally
// unreachable while nothing is open, and the framework does not quietly create a slot for it. This
// is the assertion a future "just add the arm" patch must break to be believed.
static void test_opening_an_hwcd_slot_is_what_makes_it_deliverable(void) {
  Game *g = gam();
  const uint32_t h = open_event(g, kHwCd, kHwCdSpecReady);
  drain_all(g, &h, 1);
  g->hle.deliverEvent(kHwCd, kHwCdSpecReady);
  CHECK(test_event(g, h));
  // and a spec the slot does not cover stays inert, so the mask rule is still the one under test
  drain_all(g, &h, 1);
  g->hle.deliverEvent(kHwCd, kHwCdSpecSync);
  CHECK(!test_event(g, h));
}

int main(void) {
  RUN(card_pair_delivery_fires_the_opened_slot);
  RUN(hwcd_cannot_fire_without_an_opened_slot);
  RUN(the_guest_event_table_has_no_hwcd_row);
  RUN(opening_an_hwcd_slot_is_what_makes_it_deliverable);
  return pt_summary();
}

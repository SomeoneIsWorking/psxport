// test_cdc_read_stop_completion.cpp — when a continuous read ends because the DRIVE stopped, the
// controller posts the guest a data-ready carrying the stopped status.
//
// THE DEFECT THIS GATES. `stop_continuous_read` cleared `reading`, the CdlStatRead bit,
// `first_sector_pending` and `following_sector_ready`, and posted nothing. It is called from two
// places where the drive stopped on its own — the first-sector path and the following-sector
// handoff — and from three control-command sites where the GUEST asked, so the command sites know
// (their INT3 + INT2 carry the status) and the drive sites did not. On hardware a continuous/XA
// read that runs off the end of media still raises a data-ready: the vendor backend this model is
// aligned with moves the drive out of its reading state and then raises exactly that interrupt with
// the new status (vendor/beetle-psx/mednafen/psx/cdc.c:1558-1570, CDCIRQ_DATA_END), and libcd's
// `CdlReadS` waits for precisely it. With nothing posted, the guest's wait has no way to return.
// Measured on CTR: `setloc 30:56:47 -> LBA 139097`, `setfilter file=1 chan=13`, `START streaming`,
// `EOF @ LBA 140038` — 941 sectors, ~6.3 s — and the guest's registered `CdReadyCallback`
// (`0x8001C7A4`) never ran, so the boot loop kept polling a completion that had never been posted.
//
// The guest-visible half drives the shipping `Timing` clock, the shipping controller and the
// shipping `Hle::irqPoll` through a title that declared `DeliveryOwner::GuestInterrupt`, and reads
// the answer as a CALLBACK COUNT. Every positive has a negative beside it, because a framework that
// raised nothing at all would also print a small count.
#include "testutil.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "cd_control.h"
#include "cd_drive_timing.h"
#include "cd_ready_delivery.h"
#include "cdc_command_phase.h"
#include "cdc_state.h"
#include "cdc_test_clock.h"
#include "core.h"
#include "disc.h"
#include "game.h"
#include "game_iface.h"
#include "game_runtime.h"
#include "guest_call.h"
#include "guest_cd_stream_callback_layout.h"
#include "image_identity.h"
#include "native_dispatch.h"

namespace {

constexpr uint8_t kCdlStatRead = 0x20;
constexpr uint8_t kCdlStatStandby = 0x02;
constexpr size_t kRawSectorBytes = 2352;
constexpr size_t kWholeSectorWords = 2340 / sizeof(uint32_t);
constexpr uint8_t kStreamMode = 0xA0; // whole-sector framing + double speed

std::array<uint8_t, kRawSectorBytes> make_sector(uint32_t lba) {
  std::array<uint8_t, kRawSectorBytes> raw{};
  for (size_t offset = 0; offset < raw.size(); ++offset) {
    raw[offset] = static_cast<uint8_t>((lba * 37u + offset * 11u) & 0xFFu);
  }
  return raw;
}

const std::array<uint8_t, kRawSectorBytes> kFirstSector = make_sector(16);
const std::array<uint8_t, kRawSectorBytes> kSecondSector = make_sector(17);

// ---- register-level helpers, the same ones test_cdc_continuous_read uses -------------------------

void select_bank(CdcState *cdc, uint8_t bank) {
  cdc_write(cdc, 0x1F801800u, bank);
}

uint8_t pending_irq_type(CdcState *cdc) {
  select_bank(cdc, 1);
  return static_cast<uint8_t>(cdc_read(cdc, 0x1F801803u) & 0x07u);
}

uint8_t response_byte(CdcState *cdc) {
  select_bank(cdc, 1);
  return static_cast<uint8_t>(cdc_read(cdc, 0x1F801801u));
}

void acknowledge_irq(CdcState *cdc) {
  select_bank(cdc, 1);
  cdc_write(cdc, 0x1F801803u, 0x07);
}

void write_bfrd(CdcState *cdc, uint8_t value) {
  select_bank(cdc, 0);
  cdc_write(cdc, 0x1F801803u, value);
}

void issue_command(CdcState *cdc, uint8_t command) {
  select_bank(cdc, 0);
  cdc_write(cdc, 0x1F801801u, command);
}

// ---- the disc, injected through the controller's own sector-source bindings ---------------------

uint32_t readableThrough = 17; // LBA 16..readableThrough exist; everything past it is end of media

extern "C" int test_stop_disc_read_raw(struct DiscState *, uint32_t lba, uint8_t *out, uint32_t count) {
  if (lba == 16) {
    std::copy_n(kFirstSector.data(), count, out);
    return 1;
  }
  if (lba == 17 && readableThrough >= 17) {
    std::copy_n(kSecondSector.data(), count, out);
    return 1;
  }
  return 0;
}

extern "C" int test_stop_disc_read_sector(struct DiscState *, uint32_t, uint8_t *) {
  return 0;
}

CdcState begin_read(DiscState *disc, CdcTestClock *clock, uint32_t lba = 16) {
  CdcState cdc{};
  cdc.disc = disc;
  cdc_state_init(&cdc);
  cdc.disc_read_raw_fn = test_stop_disc_read_raw;
  cdc.disc_read_sector_fn = test_stop_disc_read_sector;
  cdc_test_bind(&cdc, clock);
  cdc_set_mode(&cdc, kStreamMode);
  cdc_begin_read(&cdc, lba);
  return cdc;
}

bool acknowledge_current_sector(CdcState *cdc, CdcTestClock *clock) {
  if (cdc_test_service_deadline(cdc, clock) != 1 || pending_irq_type(cdc) != 1) {
    return false;
  }
  acknowledge_irq(cdc);
  return pending_irq_type(cdc) == 0;
}

// ---- the drive-stopped cases -------------------------------------------------------------------

// GUARD, NOT A BUG FIX. The first data-ready of a live read must carry the read bit on the wire,
// because `queue_data_ready` publishes `s->stat` as the status the guest reads. The bit IS set
// before that call today, so this asserts an existing ordering rather than repairing one — it is here
// because the ordering is invisible at the call site and silently wrong if it ever moves, and because
// the end-of-read status assertions below only make sense next to a positive for the running case.
void test_the_first_data_ready_of_a_live_read_carries_the_read_bit() {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc = begin_read(&disc, &clock);

  clock.ticks = cdc.drive_deadline_ticks;
  CHECK_EQ(cdc_drive_service(&cdc), 1);

  CHECK_EQ(pending_irq_type(&cdc), 1);
  CHECK_EQ(response_byte(&cdc) & kCdlStatRead, kCdlStatRead); // ...and it IS still reading
  CHECK_EQ(cdc.reading, 1);
}

// The FIRST sector the read owes cannot be read. The drive stops, and the guest is owed the
// data-ready that says so: type INT1, and a status byte with CdlStatRead already cleared, because
// `stop_continuous_read` clears it before posting.
void test_the_drive_stopping_on_the_first_sector_posts_an_end_of_read_completion() {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc{};
  cdc.disc = &disc;
  cdc_state_init(&cdc);
  cdc.disc_read_raw_fn = test_stop_disc_read_raw;
  cdc.disc_read_sector_fn = test_stop_disc_read_sector;
  cdc_test_bind(&cdc, &clock);
  cdc_set_mode(&cdc, kStreamMode);
  cdc_begin_read(&cdc, 900); // an LBA the image cannot supply

  CHECK_EQ(pending_irq_type(&cdc), 0);
  clock.ticks = cdc.drive_deadline_ticks;
  CHECK_EQ(cdc_drive_service(&cdc), 1);

  CHECK_EQ(pending_irq_type(&cdc), 1);            // INT1: the shape a guest's read loop waits for
  CHECK_EQ(response_byte(&cdc), kCdlStatStandby); // ...and the read bit is already gone
  CHECK_EQ(cdc.reading, 0);
  CHECK_EQ(cdc.stat & kCdlStatRead, 0);
  CHECK_EQ(cdc.first_sector_pending, 0);
  CHECK_EQ(cdc.drive_event_armed, 0);

  // The read really is over: no later clock advance manufactures another sector.
  acknowledge_irq(&cdc);
  clock.ticks += 10 * cd_drive_sector_period_cpu_ticks(kStreamMode);
  CHECK_EQ(cdc_drive_service(&cdc), 0);
  CHECK_EQ(pending_irq_type(&cdc), 0);
}

// The FOLLOWING sector: the drive announced the one it had buffered, the guest took it and asked
// for the next, and that handoff could not load. Same completion, same status, from the request-
// register path rather than the drive's own clock.
void test_the_drive_stopping_on_a_following_sector_posts_an_end_of_read_completion() {
  DiscState disc{};
  CdcTestClock clock{};
  readableThrough = 16; // LBA 17 is now the first sector past the media
  CdcState cdc = begin_read(&disc, &clock);

  CHECK(acknowledge_current_sector(&cdc, &clock));
  write_bfrd(&cdc, 0x80); // accept LBA 16
  std::array<uint32_t, kWholeSectorWords> whole_sector{};
  CHECK_EQ(cdc_dma_read(&cdc, whole_sector.data(), static_cast<int>(whole_sector.size())),
           static_cast<int>(whole_sector.size()));

  // The drive announces the sector it buffered; the guest takes the announce and hands off.
  clock.ticks = cdc.drive_deadline_ticks;
  CHECK_EQ(cdc_drive_service(&cdc), 1);
  CHECK_EQ(pending_irq_type(&cdc), 1);
  acknowledge_irq(&cdc);
  write_bfrd(&cdc, 0x80);

  CHECK_EQ(pending_irq_type(&cdc), 1);
  CHECK_EQ(response_byte(&cdc), kCdlStatStandby);
  CHECK_EQ(cdc.reading, 0);
  CHECK_EQ(cdc.following_sector_ready, 0);
  CHECK_EQ(cdc.loc_lba, 17); // the head is where the failed handoff left it

  readableThrough = 17; // restore for the other cases
}

// A guest that STOPS the read is told by its own command: INT3 answers with the status the read
// had, INT2 with the status after it. A data-ready here would be a response hardware does not
// produce for a command, and it would arrive as a sector the guest never asked for.
void test_a_guest_command_that_stops_the_read_posts_no_extra_completion() {
  for (const uint8_t command : {0x09, 0x08, 0x0A}) { // Pause, Stop, Reset
    DiscState disc{};
    CdcTestClock clock{};
    readableThrough = 17;
    CdcState cdc = begin_read(&disc, &clock);

    CHECK(acknowledge_current_sector(&cdc, &clock));
    issue_command(&cdc, command);

    clock.ticks += cdc_command_ack_delay_cpu_ticks(0);
    CHECK_EQ(cdc_drive_service(&cdc), 1);
    CHECK_EQ(pending_irq_type(&cdc), 3); // INT3: the acknowledgement
    acknowledge_irq(&cdc);
    clock.ticks = cdc.command_deadline_ticks;
    CHECK_EQ(cdc_drive_service(&cdc), 1);
    CHECK_EQ(pending_irq_type(&cdc), 2); // INT2: the completion
    acknowledge_irq(&cdc);

    CHECK_EQ(cdc.reading, 0);
    CHECK_EQ(pending_irq_type(&cdc), 0); // nothing else: no invented data-ready
  }
}

// A full response queue must not turn the end-of-read into a silent truncation. The read still
// stops — the guest's own status poll says so — and no response is fabricated for a queue that
// cannot hold one.
void test_a_full_response_queue_leaves_the_end_of_read_unannounced_and_reported() {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc = begin_read(&disc, &clock);
  CHECK(acknowledge_current_sector(&cdc, &clock));
  while (cdc_post_data_ready(&cdc) != 0) {
  }
  CHECK_EQ(pending_irq_type(&cdc), 1); // the queue is full of data-readies

  // LBA 17 no longer resolves, and the handoff that would load it fails.
  readableThrough = 16;
  write_bfrd(&cdc, 0x80);
  std::array<uint32_t, kWholeSectorWords> whole_sector{};
  cdc_dma_read(&cdc, whole_sector.data(), static_cast<int>(whole_sector.size()));
  clock.ticks = cdc.drive_deadline_ticks;
  cdc_drive_service(&cdc);
  acknowledge_irq(&cdc);
  write_bfrd(&cdc, 0x80);

  CHECK_EQ(cdc.reading, 0); // the drive stopped even though it could not say so
  CHECK_EQ(cdc.stat & kCdlStatRead, 0);
  readableThrough = 17;
}

// ---- the guest-visible half: the completion reaches a declared title's ready callback ------------

constexpr uint32_t kCallbackSlot = 0x80012000u;
constexpr uint32_t kCallback = 0x80012100u;
constexpr uint32_t kIMask = 0x1F801074u;
constexpr uint32_t kStreamLba = 100;
constexpr uint32_t kStreamSectors = 5;

int readyCallbackCalls = 0;
uint32_t readyCallbackStatus = 0;

extern "C" int test_stream_disc_read_raw(struct DiscState *, uint32_t lba, uint8_t *out, uint32_t count) {
  if (lba < kStreamLba || lba >= kStreamLba + kStreamSectors) {
    return 0; // past the end of the media the drive can supply
  }
  for (uint32_t i = 0; i < count; ++i) {
    out[i] = static_cast<uint8_t>(lba * 31u + i);
  }
  out[15] = 1u; // Mode 1 data sector: the drive presents it, so this is a DATA stream
  return 1;
}

// The guest's registered ready callback, in the shape stock libcd's per-sector path has: take the
// sector the controller has for it, then ask for the next one. It is the request that makes the
// following-sector handoff happen, so it is part of the seam rather than an incidental detail.
void readyCallback(Core *core) {
  ++readyCallbackCalls;
  readyCallbackStatus = core->r[4];
  CdcState &controller = core->game->cdc;
  const int savedBank = controller.index;
  std::array<uint32_t, kWholeSectorWords> sector{};
  (void)cdc_dma_read(&controller, sector.data(), static_cast<int>(sector.size()));
  cdc_write(&controller, 0u, 0u);
  cdc_write(&controller, 3u, 0x80u); // request the next sector
  cdc_write(&controller, 0u, static_cast<uint8_t>(savedBank));
}

class StreamRuntime final : public GameRuntime {
public:
  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void registerOverrides(Game &) override {}
  void bootInit(Core &) override {}
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }
  const GuestCdStreamCallbackLayout *guestCdStreamCallbackLayout() const override {
    return &layout;
  }

  GuestCdStreamCallbackLayout layout{kCallbackSlot, GuestCdStreamCallbackLayout::DeliveryOwner::GuestInterrupt};
};

void configureStream(Game &game) {
  const auto image =
      game.core.imageCatalog().activate("test-main", {kCallback & 0x1FFFFFFFu, (kCallback & 0x1FFFFFFFu) + 4u}, 1u);
  CHECK(game.core.nativeDispatcher().install({{image, kCallback}, "cd-stream-ready-callback", readyCallback}));
  game.core.mem_w32(kCallbackSlot, kCallback);
  game.core.mem_w32(kIMask, 1u << 2u); // the CD line this title opens, as stock CdInit leaves it
  readyCallbackCalls = 0;
  readyCallbackStatus = 0;
  game.cdc.disc_read_raw_fn = test_stream_disc_read_raw;
  cdc_set_mode(&game.cdc, kStreamMode);
  cdc_begin_read(&game.cdc, kStreamLba);
  game.cd.stream_active = 1;
}

std::unique_ptr<Game> startStream() {
  static StreamRuntime runtime;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
  configureStream(*game);
  return game;
}

// The status byte the controller put on the wire is asserted at the REGISTER seam, by
// `response_byte`, in the drive-stopped cases above — never from inside the guest callback. The
// delivery drops the current response (`consumeCurrentResponse` calls `cdc_take_current_response`,
// which advances `q_head`) BEFORE it notifies the callback, so a callback indexing `q[q_head]` read
// the NEXT, not-yet-written entry. That is a test reading the wrong thing and asserting the wrong
// answer; the wire the guest actually reads is the register.

// The whole read, on the shipping clock: one host step per sector period, polling after each.
//
// THE READ ENDING AND THE COMPLETION BEING DELIVERED ARE NOT THE SAME STEP, and a test that
// conflates them measures the wrong thing. The guest's own callback is what runs out of media: it
// asks for the sector after the last one, the drive cannot load it, and the end-of-read completion
// is posted from inside that callback — which is itself running inside the poll that delivered the
// data-ready. So the poll in which `reading` falls to zero has an end-of-read completion still
// OWED, and the next poll is what consumes it. Measured: 5 sectors, 6 data-readies delivered, then
// `reading` 1 -> 0 during the sixth, and the seventh callback carries the end.
//
// So the driver drains: it polls until the read has stopped AND the controller owes nothing.
int runStreamToItsEnd() {
  auto game = startStream();
  const uint32_t period = cd_drive_sector_period_cpu_ticks(kStreamMode);
  for (int step = 0; step < 4 * static_cast<int>(kStreamSectors) + 8; ++step) {
    game->timing.advanceGuestInstructionTicks(period);
    game->hle.irqPoll(&game->core);
    if (game->cdc.reading == 0 && cdc_current_irq_type(&game->cdc) == 0) {
      break;
    }
  }
  const int calls = readyCallbackCalls;
  delete game.release();
  return calls;
}

// THE POSITIVE: a guest that declared `GuestInterrupt` is told, through the shipping poll, that its
// read ended — the end-of-read completion reaches its registered callback, which on this build was
// unreachable, leaving the guest's `CdlReadS` wait with nothing that could ever return it.
//
// The count is the DRIVE'S obligation and not a poll count: it is told once per sector the drive
// announced, plus the end. It is not asserted exactly, because the callback's own request is what
// discovers the end of media and that request is answered too; an exact number would be a
// restatement of the implementation rather than of the contract.
void test_a_stream_that_runs_off_the_media_delivers_its_end_to_the_guest() {
  const int calls = runStreamToItsEnd();

  CHECK_EQ(readyCallbackStatus, 1u); // the layout's declared libcd data-ready code
  CHECK(calls >= static_cast<int>(kStreamSectors) + 1);
}

// NEGATIVE: the completion is a data-ready like any other, so once it is consumed it is consumed
// FOREVER — however often the guest is polled. The count is the drive's, not the poll count's.
void test_the_end_of_read_completion_is_delivered_exactly_once() {
  auto game = startStream();
  const uint32_t period = cd_drive_sector_period_cpu_ticks(kStreamMode);
  for (int step = 0; step < 4 * static_cast<int>(kStreamSectors) + 8; ++step) {
    game->timing.advanceGuestInstructionTicks(period);
    game->hle.irqPoll(&game->core);
    if (game->cdc.reading == 0 && cdc_current_irq_type(&game->cdc) == 0) {
      break;
    }
  }
  CHECK_EQ(game->cdc.reading, 0);
  CHECK_EQ(cdc_current_irq_type(&game->cdc), 0); // the run ended with nothing owed
  const int drained = readyCallbackCalls;
  for (int poll = 0; poll < 8; ++poll) {
    game->hle.irqPoll(&game->core);
  }
  CHECK_EQ(readyCallbackCalls, drained);
  CHECK_EQ(cdc_current_irq_type(&game->cdc), 0);
  delete game.release();
}

// NEGATIVE: a read that is still running has owed no end-of-read completion, so polling it must
// deliver only the per-sector data-readies. This is the shape a framework that posted the terminal
// completion eagerly — at the first sector — would fail.
//
// It also pins the FIRST data-ready's status, which is where a separate ordering bug lived: the read
// bit used to be set one line BELOW the announcement, so the first sector of every read was
// published telling the guest the read had already stopped.
void test_a_running_read_owes_no_end_of_read_completion() {
  auto game = startStream();
  const uint32_t period = cd_drive_sector_period_cpu_ticks(kStreamMode);
  game->timing.advanceGuestInstructionTicks(period);
  game->hle.irqPoll(&game->core);
  CHECK_EQ(readyCallbackCalls, 1); // the first sector's data-ready, and nothing else
  CHECK_EQ(game->cdc.reading, 1);
  delete game.release();
}

} // namespace

int main() {
  RUN(the_first_data_ready_of_a_live_read_carries_the_read_bit);
  RUN(the_drive_stopping_on_the_first_sector_posts_an_end_of_read_completion);
  RUN(the_drive_stopping_on_a_following_sector_posts_an_end_of_read_completion);
  RUN(a_guest_command_that_stops_the_read_posts_no_extra_completion);
  RUN(a_full_response_queue_leaves_the_end_of_read_unannounced_and_reported);
  RUN(a_stream_that_runs_off_the_media_delivers_its_end_to_the_guest);
  RUN(the_end_of_read_completion_is_delivered_exactly_once);
  RUN(a_running_read_owes_no_end_of_read_completion);
  return pt_summary();
}

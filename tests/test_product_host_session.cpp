// ProductHost and TitleSession end to end over a stub catalog and FrameDriver: how a run ends, what
// it counts, and what a picker panel is not allowed to do.
#include "testutil.h"

#include "c_subsys.h"
#include "config_vars.h"
#include "core.h"
#include "dbg_server.h"
#include "frame_presenter.h"
#include "game.h"
#include "game_runtime.h"
#include "guest_program_image.h"
#include "host_input.h"
#include "platform_hle.h"
#include "product_host.h"
#include "state/state_command.h"
#include "title_catalog.h"

#include <SDL3/SDL.h>
#include <lucent/content.h>
#include <lucent/log.h>

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <netinet/in.h>
#include <span>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t kTextAddress = 0x80010000u;
constexpr std::uint32_t kTextSize = 0x800u;
constexpr std::uint32_t kStackAddress = 0x801FFFF0u;
constexpr std::uint32_t kVSyncAddress = 0x00014000u;
constexpr std::uint32_t kStackTopWord = 0x80010100u;
constexpr std::uint32_t kStackReserveWord = 0x80010104u;

void writeU32(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned byte = 0; byte < 4; ++byte) {
    bytes[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8u));
  }
}

// A PS-X EXE whose text starts with `jr ra; nop`, so the libc init the crt0 calls returns at once,
// and holds the two words the crt0 derives the heap size from.
std::vector<std::uint8_t> executable() {
  std::vector<std::uint8_t> bytes(0x800u + kTextSize);
  constexpr std::array<std::uint8_t, 8> magic{'P', 'S', '-', 'X', ' ', 'E', 'X', 'E'};
  std::copy(magic.begin(), magic.end(), bytes.begin());
  writeU32(bytes, 0x10u, kTextAddress);
  writeU32(bytes, 0x18u, kTextAddress);
  writeU32(bytes, 0x1Cu, kTextSize);
  writeU32(bytes, 0x30u, kStackAddress);
  writeU32(bytes, 0x800u, 0x03E00008u);
  writeU32(bytes, 0x800u + (kStackTopWord - kTextAddress), 0x001FFFF0u);
  writeU32(bytes, 0x800u + (kStackReserveWord - kTextAddress), 0x00010000u);
  return bytes;
}

// What the stub driver does, and what the test reads back. Step numbers count every step of every
// session; the `product*` fields count one product session's own steps.
struct Script {
  unsigned steps = 0;
  unsigned quitAtStep = 0;  // a client `quit` arrives during this step
  unsigned closeAtStep = 0; // the window is closed during this step
  bool panelCloses = false; // the next picker panel step closes the window
  unsigned productSessions = 0;
  unsigned firstProductEndsAt = 0;    // the first product session ends its own run at this step
  unsigned secondProductClosesAt = 0; // the second product session closes the window at this step
  std::filesystem::path saveStateTo;  // a product saves its machine here after its first step
  std::filesystem::path consumeState; // the first product deletes this file at its first step
  std::string card;                   // the memory card every session opens, so a state can carry it
};

void closeWindow() {
  SDL_Event close{};
  close.type = SDL_EVENT_QUIT;
  SDL_PushEvent(&close);
}

class FenceBackend final : public psx::frame::FramePresentationBackend {
public:
  void emit(std::span<const RqItem>) override {}
  void presentReal() override {}
  void captureDiagnostic(std::uint64_t, bool) override {}
  void pace(int, int) override {}
  void reconcile(std::uint64_t) override {}
  void beginLedgerFrame() override {}
  bool interpolatesRecords() const override {
    return false;
  }
  bool sealedFrameIsCut() override {
    return false;
  }
  void presentInBetween() override {}
  psx::gpu::RecordRect displayedBuffer() override {
    return {};
  }
};

class Driver final : public FrameDriver {
public:
  Driver(Game &game, Script &script) : game_(game), script_(script) {}

  void stepFrame(Core &core, std::uint32_t) override {
    ++script_.steps;
    // A panel's player input is suppressed; the product's is live.
    if (game_.pad.playerInputSuppressed()) {
      if (script_.panelCloses) {
        script_.panelCloses = false;
        closeWindow();
      }
      game_.presentation.commitUnpresented(backend_);
      return;
    }
    if (session_ == 0) {
      session_ = ++script_.productSessions;
    }
    ++steps_;
    if (script_.steps == script_.quitAtStep) {
      game_.dbg_server.requestQuit();
    }
    if (script_.steps == script_.closeAtStep || (session_ == 2 && steps_ == script_.secondProductClosesAt)) {
      closeWindow();
      game_.hostInput.drainEvents();
    }
    if (steps_ == 1 && !script_.saveStateTo.empty()) {
      std::string error;
      CHECK(psx::state::save(core, script_.saveStateTo.string(), error));
    }
    if (session_ == 1 && steps_ == 1 && !script_.consumeState.empty()) {
      std::filesystem::remove(script_.consumeState);
    }
    if (session_ == 1 && steps_ == script_.firstProductEndsAt) {
      game_.run.requestEnd();
    }
    game_.presentation.commitUnpresented(backend_);
  }

private:
  Game &game_;
  Script &script_;
  FenceBackend backend_;
  unsigned session_ = 0;
  unsigned steps_ = 0;
};

class Runtime final : public GameRuntime {
public:
  explicit Runtime(Script &script) : script_(script) {
    plan_.vsyncAddress = kVSyncAddress;
    plan_.windowLo[0] = kVSyncAddress;
    plan_.windowHi[0] = kVSyncAddress + 4u;
    image_.bss = {0x80011000u, 0x80011100u};
    image_.stackTopWordAddress = kStackTopWord;
    image_.stackReserveWordAddress = kStackReserveWord;
    image_.heapBase = 0x80020000u;
    image_.globalPointer = 0x80018000u;
    image_.libcInitEntry = kTextAddress;
    image_.stackBias.declared = true;
    image_.stackBias.bytes = -8;
    image_.residentText = {kTextAddress & 0x1FFFFFFFu, (kTextAddress + kTextSize) & 0x1FFFFFFFu};
  }
  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void registerOverrides(Game &game) override {
    if (!script_.card.empty()) {
      game.memcard.init();
    }
  }
  void bootInit(Core &) override {}
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }
  const HostIdentity *hostIdentity() const override {
    identity_.cardDefaultPath = script_.card.c_str();
    return &identity_;
  }
  const PlatformHlePlan *platformHlePlan() const override {
    return &plan_;
  }
  const GuestProgramImage *guestProgramImage() const override {
    return &image_;
  }
  std::unique_ptr<FrameDriver> createFrameDriver(Game &game) override {
    return std::make_unique<Driver>(game, script_);
  }

private:
  Script &script_;
  mutable HostIdentity identity_{};
  PlatformHlePlan plan_{};
  GuestProgramImage image_{};
};

// Two titles over the same executable bytes, so a picker has two panels; or only the first.
class Catalog final : public psx::host::TitleCatalog {
public:
  Catalog(Script &script, const std::string &sha256, std::filesystem::path root, std::size_t count = 2)
      : sha256_(sha256), root_(std::move(root)), count_(count), runtimes_{Runtime(script), Runtime(script)} {
    titles_[0] = identity("One", "SCUS_000.01", "alpha");
    titles_[1] = identity("Two", "SCUS_000.02", "beta");
  }
  std::string_view productName() const override {
    return "Product host test";
  }
  std::filesystem::path provisioningRoot() const override {
    return root_;
  }
  std::span<const psx::host::TitleIdentity> titles() const override {
    return std::span(titles_).first(count_);
  }
  GameRuntime &runtime(std::size_t index) const override {
    return runtimes_[index];
  }

private:
  psx::host::TitleIdentity identity(const char *name, const char *serial, const char *slug) const {
    return psx::host::TitleIdentity{.displayName = name,
                                    .serial = serial,
                                    .slug = slug,
                                    .fileSize = 0x800u + kTextSize,
                                    .sha256 = sha256_,
                                    .entry = kTextAddress,
                                    .globalPointer = 0u,
                                    .textAddress = kTextAddress,
                                    .textSize = kTextSize,
                                    .stackAddress = kStackAddress,
                                    .stackOffset = 0u};
  }

  std::string sha256_;
  std::filesystem::path root_;
  std::size_t count_;
  mutable std::array<Runtime, 2> runtimes_;
  std::array<psx::host::TitleIdentity, 2> titles_{};
};

struct Fixture {
  std::filesystem::path root;
  std::filesystem::path alpha;
  std::string sha256;
  std::vector<std::string> lines;

  Fixture() {
    root = std::filesystem::current_path() / ("product_host_session_" + std::to_string(::getpid()));
    const std::vector<std::uint8_t> bytes = executable();
    sha256 = lucent::content::sha256_hex(lucent::content::sha256(std::as_bytes(std::span(bytes))));
    for (const char *path : {"alpha/SCUS_000.01", "beta/SCUS_000.02"}) {
      const std::filesystem::path file = root / path;
      std::filesystem::create_directories(file.parent_path());
      std::ofstream(file, std::ios::binary)
          .write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    alpha = root / "alpha/SCUS_000.01";
    lucent::set_sink([this](lucent::Level, std::string_view line) {
      lines.emplace_back(line);
    });
  }
  ~Fixture() {
    lucent::set_sink(nullptr);
    psx::config::cv_native_frames.clear(psx::config::Layer::Runtime);
    psx::config::cv_load_state.clear(psx::config::Layer::Runtime);
    psx::config::cv_debug_server.clear(psx::config::Layer::Runtime);
    std::filesystem::remove_all(root);
  }

  int count(const char *needle) const {
    int found = 0;
    for (const std::string &line : lines) {
      found += line.find(needle) != std::string::npos ? 1 : 0;
    }
    return found;
  }
};

// The host's command line: `run(host)` is the bare product, `run(host, {path})` an executable.
int run(psx::host::ProductHost &host, std::vector<std::string> arguments = {}) {
  arguments.insert(arguments.begin(), "product_host_test");
  std::vector<char *> argv;
  argv.reserve(arguments.size());
  for (std::string &argument : arguments) {
    argv.push_back(argument.data());
  }
  return host.run(static_cast<int>(argv.size()), argv.data());
}

bool g_finished = false;

// A window close used to end the process with exit(0) from inside the event drain, which would end
// this test green with its later cases unrun.
void refuseEarlyExit() {
  if (!g_finished) {
    fprintf(stderr, "FAIL: the process exited before the tests finished\n");
    _exit(1);
  }
}

} // namespace

static void test_a_session_ends_on_its_cap_counted_in_fence_steps(void) {
  Fixture fixture;
  Script script;
  const Catalog catalog(script, fixture.sha256, fixture.root);
  psx::config::cv_native_frames.set(psx::config::Layer::Runtime, 5);
  {
    psx::host::ProductHost host(catalog);
    CHECK_EQ(run(host, {fixture.alpha.string()}), 0);
  }
  CHECK_EQ(script.steps, 5u);
  CHECK_EQ(fixture.count("run complete: product_steps=5 presentation_fences=5 "), 1);
  CHECK_EQ(fixture.count("reached its run budget of 5 step(s)"), 1);
}

static void test_a_client_quit_reaches_the_run_end_report(void) {
  Fixture fixture;
  Script script;
  script.quitAtStep = 3;
  const Catalog catalog(script, fixture.sha256, fixture.root);
  psx::config::cv_native_frames.set(psx::config::Layer::Runtime, 50);
  {
    psx::host::ProductHost host(catalog);
    CHECK_EQ(run(host, {fixture.alpha.string()}), 0);
  }
  CHECK_EQ(script.steps, 3u);
  CHECK_EQ(fixture.count("was asked to quit after 3 step(s)"), 1);
  CHECK_EQ(fixture.count("run complete: product_steps=3 "), 1);
  CHECK_EQ(fixture.count("[playcheck] presents:"), 1);
}

static void test_a_window_close_reaches_the_run_end_report(void) {
  Fixture fixture;
  Script script;
  script.closeAtStep = 4;
  const Catalog catalog(script, fixture.sha256, fixture.root);
  psx::config::cv_native_frames.set(psx::config::Layer::Runtime, 50);
  {
    psx::host::ProductHost host(catalog);
    CHECK_EQ(run(host, {fixture.alpha.string()}), 0);
  }
  CHECK_EQ(script.steps, 4u);
  CHECK_EQ(fixture.count("was asked to quit after 4 step(s)"), 1);
  CHECK_EQ(fixture.count("run complete: product_steps=4 "), 1);
}

// The selector boots both panels and runs until the budget ends it. A panel that loaded the
// configured state would end the process on the bad path, and one that reported would print a run
// line; the watchdog the host armed is still armed when the panels are gone.
static void test_panels_report_nothing_load_no_state_and_leave_the_watchdog_armed(void) {
  Fixture fixture;
  Script script;
  const Catalog catalog(script, fixture.sha256, fixture.root);
  psx::config::cv_native_frames.set(psx::config::Layer::Runtime, 8);
  psx::config::cv_load_state.set(psx::config::Layer::Runtime, "no/such/state.sav");
  {
    psx::host::ProductHost host(catalog);
    CHECK(!watchdog_armed());
    CHECK_EQ(run(host), 0);
    CHECK(watchdog_armed());
  }
  CHECK(!watchdog_armed());
  CHECK(script.steps > 0u);
  CHECK_EQ(fixture.count("booting panel 0"), 1);
  CHECK_EQ(fixture.count("booting panel 1"), 1);
  CHECK_EQ(fixture.count("selector ended after 8 frame(s)"), 1);
  CHECK_EQ(fixture.count("run complete"), 0);
}

static void test_a_window_close_in_the_selector_ends_the_process_cleanly(void) {
  Fixture fixture;
  Script script;
  script.panelCloses = true;
  const Catalog catalog(script, fixture.sha256, fixture.root);
  psx::config::cv_native_frames.set(psx::config::Layer::Runtime, 200);
  {
    psx::host::ProductHost host(catalog);
    CHECK_EQ(run(host), 0);
    CHECK(watchdog_armed());
  }
  CHECK(script.steps > 0u);
  CHECK_EQ(fixture.count("selector asked to quit after"), 1);
  CHECK_EQ(fixture.count("selector ended after"), 0);
  CHECK_EQ(fixture.count("run complete"), 0);
}

// The control channel's `pick`, sent until the selector has started `count` titles. A product answers
// it as an unknown command, so a send that reaches the running title is simply repeated.
class PickClient {
public:
  PickClient(int port, int count)
      : thread_([this, port, count] {
          run(port, count);
        }) {}
  ~PickClient() {
    done_ = true;
    thread_.join();
  }
  int started() const {
    return started_;
  }

private:
  void run(int port, int count) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    int fd = -1;
    while (!done_ && started_ < count && std::chrono::steady_clock::now() < deadline) {
      if (fd < 0) {
        fd = connectTo(port);
        if (fd < 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          continue;
        }
      }
      const std::string reply = ask(fd, "pick alpha\n");
      if (reply.empty()) {
        close(fd);
        fd = -1;
        continue;
      }
      if (reply.find("ok: starting alpha") != std::string::npos) {
        ++started_;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (fd >= 0) {
      close(fd);
    }
  }

  static int connectTo(int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0) {
      close(fd);
      return -1;
    }
    return fd;
  }

  // One command's reply, up to the server's delimiter; empty when the connection dropped.
  static std::string ask(int fd, const char *line) {
    if (write(fd, line, std::strlen(line)) < 0) {
      return {};
    }
    std::string reply;
    char buffer[512];
    while (reply.find("---END---") == std::string::npos) {
      const ssize_t got = read(fd, buffer, sizeof buffer);
      if (got <= 0) {
        return {};
      }
      reply.append(buffer, static_cast<std::size_t>(got));
    }
    return reply;
  }

  std::atomic<bool> done_{false};
  std::atomic<int> started_{0};
  std::thread thread_;
};

// Ending its own run sends the title back to the selector, and only the process's first product
// session loads PSXPORT_LOAD_STATE: the first deletes the state file, so a second load would fail
// and end the process.
static void test_a_title_that_ends_its_run_returns_to_the_selector_and_loads_no_state_again(void) {
  Fixture fixture;
  Script script;
  const std::filesystem::path state = fixture.root / "first.state";
  script.saveStateTo = state;
  script.card = (fixture.root / "card.mcr").string();
  const Catalog catalog(script, fixture.sha256, fixture.root);
  psx::config::cv_native_frames.set(psx::config::Layer::Runtime, 2);
  {
    psx::host::ProductHost host(catalog);
    CHECK_EQ(run(host, {fixture.alpha.string()}), 0);
  }
  CHECK(std::filesystem::exists(state));

  script.saveStateTo.clear();
  script.consumeState = state;
  script.firstProductEndsAt = 2;
  script.secondProductClosesAt = 2;
  script.productSessions = 0;
  const int port = 20000 + static_cast<int>(::getpid() % 20000);
  psx::config::cv_debug_server.set(psx::config::Layer::Runtime, std::to_string(port));
  psx::config::cv_native_frames.set(psx::config::Layer::Runtime, 3000);
  psx::config::cv_load_state.set(psx::config::Layer::Runtime, state.string());
  {
    psx::host::ProductHost host(catalog);
    PickClient client(port, 2);
    CHECK_EQ(run(host), 0);
  }
  CHECK_EQ(script.productSessions, 2u);
  CHECK(!std::filesystem::exists(state));
  CHECK_EQ(fixture.count("One ended its run after 2 step(s)"), 1);
  CHECK_EQ(fixture.count("One was asked to quit after 2 step(s)"), 1);
  CHECK_EQ(fixture.count("run complete: product_steps=2 "), 3);
}

static void test_help_names_the_product_and_boots_nothing(void) {
  Fixture fixture;
  Script script;
  const Catalog catalog(script, fixture.sha256, fixture.root);
  {
    psx::host::ProductHost host(catalog);
    CHECK_EQ(run(host, {"--help"}), 0);
    CHECK_EQ(run(host, {"-h"}), 0);
    CHECK(!watchdog_armed());
  }
  CHECK_EQ(script.steps, 0u);
  CHECK_EQ(fixture.count("run budget:"), 0);
  CHECK_EQ(fixture.count("Usage: product_host_test [executable]"), 2);
  CHECK_EQ(fixture.count("the Product host test title selector over "), 2);
  CHECK_EQ(fixture.count("  Two (SCUS_000.02)"), 2);
}

static void test_unexpected_arguments_are_refused(void) {
  Fixture fixture;
  Script script;
  const Catalog catalog(script, fixture.sha256, fixture.root);
  {
    psx::host::ProductHost host(catalog);
    CHECK_EQ(run(host, {"--frames"}), 2);
    CHECK_EQ(run(host, {fixture.alpha.string(), "extra"}), 2);
  }
  CHECK_EQ(script.steps, 0u);
  CHECK_EQ(fixture.count("unexpected arguments"), 2);
}

// One title needs no selector: the bare product boots the provisioned executable directly.
static void test_a_single_title_catalog_skips_the_selector(void) {
  Fixture fixture;
  Script script;
  const Catalog catalog(script, fixture.sha256, fixture.root, 1);
  psx::config::cv_native_frames.set(psx::config::Layer::Runtime, 3);
  {
    psx::host::ProductHost host(catalog);
    CHECK_EQ(run(host), 0);
  }
  CHECK_EQ(script.steps, 3u);
  CHECK_EQ(fixture.count("booting panel"), 0);
  CHECK_EQ(fixture.count("run complete: product_steps=3 "), 1);
}

int main(void) {
  std::atexit(refuseEarlyExit);
  // The product boot clears the display, which presents through a real (headless) GPU device.
  SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_OVERRIDE);
  SDL_Init(SDL_INIT_EVENTS);
  RUN(help_names_the_product_and_boots_nothing);
  RUN(unexpected_arguments_are_refused);
  RUN(a_single_title_catalog_skips_the_selector);
  RUN(a_session_ends_on_its_cap_counted_in_fence_steps);
  RUN(a_client_quit_reaches_the_run_end_report);
  RUN(a_window_close_reaches_the_run_end_report);
  RUN(panels_report_nothing_load_no_state_and_leave_the_watchdog_armed);
  RUN(a_window_close_in_the_selector_ends_the_process_cleanly);
  // Last: the control channel it opens listens for the rest of the process.
  RUN(a_title_that_ends_its_run_returns_to_the_selector_and_loads_no_state_again);
  g_finished = true;
  return pt_summary();
}

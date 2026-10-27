#include "dbg_server.h"

#include "command_args.h"
#include "core.h"
#include "debug_commands.h"
#include "game.h"
#include "gpu_device.h"
#include "watchdog.h"

#include <unistd.h>

#include <lucent/http.h>
#include <lucent/log.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace {

// The process-lifetime handoff between the listener thread and whichever Game holds the claim.
// The thread never ends, so nothing it touches may belong to one Game.
class DebugChannel {
public:
  static DebugChannel process;

  bool claim(DbgServer *server) {
    const std::lock_guard lock(mutex_);
    if (owner_ != nullptr && owner_ != server) {
      return false;
    }
    owner_ = server;
    return true;
  }

  void release(const DbgServer *server) {
    const std::lock_guard lock(mutex_);
    if (owner_ == server) {
      owner_ = nullptr;
    }
  }

  bool ownedBy(const DbgServer *server) {
    const std::lock_guard lock(mutex_);
    return owner_ == server;
  }

  void requestQuit() {
    const std::lock_guard lock(mutex_);
    if (owner_ != nullptr) {
      owner_->requestQuit();
    }
  }

  // The first call opens the loopback listener on `port`; it then serves for the process's life.
  void listen(int port) {
    const std::lock_guard lock(mutex_);
    if (server_ != nullptr) {
      return;
    }
    server_ =
        std::make_unique<lucent::http::Server>(lucent::http::ServerOptions{.port = static_cast<std::uint16_t>(port)},
                                               [this](const lucent::http::Request &request) {
                                                 return answer(request);
                                               });
    if (server_->start()) {
      lucent::info("dbgsrv", "listening on http://127.0.0.1:{}/command (PSXPORT_DEBUG_SERVER)", port);
    } else {
      lucent::error("dbgsrv", "could not listen on 127.0.0.1:{}", port);
    }
  }

  // Main thread: run the queued command, if any, outside the lock.
  template <class Run> void service(Run &&run) {
    std::unique_lock lock(mutex_);
    if (!requestPending_) {
      return;
    }
    // A request abandoned while it runs bumps the generation, and its reply is dropped.
    const std::uint64_t generation = requestGeneration_;
    const std::string command = command_;
    lock.unlock();
    std::string reply;
    run(command.c_str(), reply);
    lock.lock();
    requestPending_ = false;
    if (generation == requestGeneration_) {
      reply_ = std::move(reply);
      replyGeneration_ = generation;
      replyReady_ = true;
    }
    changed_.notify_all();
  }

  // Listener thread: queue one command and wait for its reply, abandoning it after a bounded wait.
  std::string submit(std::string_view line) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    std::unique_lock lock(mutex_);
    if (!changed_.wait_until(lock, deadline, [this] {
          return !requestPending_ && !(replyReady_ && replyGeneration_ == requestGeneration_);
        })) {
      return std::string(kTimedOut);
    }
    command_.assign(line);
    ++requestGeneration_;
    requestPending_ = true;
    replyReady_ = false;
    if (!changed_.wait_until(lock, deadline, [this] {
          return replyReady_ && replyGeneration_ == requestGeneration_;
        })) {
      requestPending_ = false;
      ++requestGeneration_;
      replyReady_ = false;
      changed_.notify_all();
      return std::string(kTimedOut);
    }
    replyReady_ = false;
    changed_.notify_all();
    return std::move(reply_);
  }

private:
  static constexpr std::string_view kTimedOut = "(debug server: command timed out; game frozen or busy)\n";

  std::mutex mutex_;
  std::condition_variable changed_;
  // POST /command with one command line as the body; the reply is the command's text.
  lucent::http::Response answer(const lucent::http::Request &request) {
    if (request.method != "POST" || request.path() != "/command") {
      return lucent::http::Response::text(404, "Not Found", "POST one command line to /command\n");
    }
    std::string_view line = request.body;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
      line.remove_suffix(1);
    }
    if (line == "quit" || line == "q" || line == "exit") {
      requestQuit();
      return lucent::http::Response::text(200, "OK", "quitting\n");
    }
    return lucent::http::Response::text(200, "OK", line.empty() ? std::string() : submit(line));
  }

  DbgServer *owner_ = nullptr;
  std::unique_ptr<lucent::http::Server> server_;
  std::string command_;
  bool requestPending_ = false;
  std::uint64_t requestGeneration_ = 0;
  std::string reply_;
  bool replyReady_ = false;
  std::uint64_t replyGeneration_ = 0;
};

DebugChannel DebugChannel::process;

inline constexpr std::array<psx::debug::VerbUsage, 5> kServerVerbs{{
    {"pause", "freeze the game; the window holds the last frame"},
    {"step [n]", "advance n frames, then freeze again (default 1)"},
    {"play | resume", "unfreeze"},
    {"frame", "present counters, pause state and display origin"},
    {"quit | q | exit", "end the run"},
}};

} // namespace

DbgServer::~DbgServer() {
  DebugChannel::process.release(this);
}

bool DbgServer::claimEndpoint() {
  return DebugChannel::process.claim(this);
}

void DbgServer::start() {
  const int port = psx::config::debug_server_port();
  if (port == 0 || !DebugChannel::process.claim(this)) {
    return;
  }
  DebugChannel::process.listen(port);
}

void DbgServer::runCommand(Core &core, const char *line, std::string &reply) {
  const psx::debug::ReplySink sink = [&reply](std::string_view text) {
    reply.append(text);
    reply.push_back('\n');
  };
  const psx::debug::CommandArgs args(line);
  const std::string_view cmd = args.word(0);
  if (cmd == "help") {
    psx::debug::DebugCommands::help(core, kServerVerbs, sink);
  } else if (cmd == "pause") {
    mPaused = true;
    mStep = 0;
    sink(lucent::format("paused at frame {}", core.game->display.frame()));
  } else if (cmd == "play" || cmd == "resume") {
    mPaused = false;
    mStep = 0;
    sink("resumed");
  } else if (cmd == "step") {
    const std::uint32_t asked = args.number(1).value_or(0);
    const std::uint32_t frames = asked != 0 ? asked : 1u;
    addStep(static_cast<int>(frames));
    sink(lucent::format("step +{} (frame {})", frames, core.game->display.frame()));
  } else if (cmd == "frame") {
    // `frame` counts real presents; interpolated in-betweens are counted apart.
    const int real = core.game->display.frame();
    const int interpolated = core.game->display.interpolatedFrames();
    const psx::gpu::DisplayArea area = core.gpuDevice().displayArea();
    sink(lucent::format("frame={} interp={} total={} paused={} disp=({},{})",
                        real,
                        interpolated,
                        real + interpolated,
                        mPaused ? 1 : 0,
                        area.x,
                        area.y));
  } else if (!core.game->debugCommands.run(core, args, sink)) {
    sink(lucent::format("? {}  (try 'help')", line));
  }
}

void DbgServer::service(Core *c) {
  if (!DebugChannel::process.ownedBy(this)) {
    return;
  }
  DebugChannel::process.service([this, c](const char *line, std::string &reply) {
    runCommand(*c, line, reply);
  });
}

// A pause re-shows the last real frame and never re-renders.
void DbgServer::honourPause(Core *c) {
  if (!mPaused) {
    return;
  }
  psx::platform::Watchdog::suspend();
  while (mPaused) {
    if (mStep > 0) {
      mStep--;
      break;
    }
    idleFrame(c);
  }
  psx::platform::Watchdog::resume();
}

void DbgServer::idleFrame(Core *c) {
  c->game->pad.pollHostInput(); // must not tick the pad-frame clock
  c->game->display.repaint();
  service(c);
  usleep(15000);
}

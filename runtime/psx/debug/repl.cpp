#include "repl.h"

#include "command_args.h"
#include "core.h"
#include "debug_commands.h"
#include "game.h"
#include "watchdog.h"

#include <lucent/log.h>

#include <array>
#include <cstdio>
#include <optional>
#include <string_view>

namespace {

class WatchdogInputWait {
public:
  WatchdogInputWait() {
    psx::platform::Watchdog::suspend();
  }
  ~WatchdogInputWait() {
    psx::platform::Watchdog::resume();
  }
  WatchdogInputWait(const WatchdogInputWait &) = delete;
  WatchdogInputWait &operator=(const WatchdogInputWait &) = delete;
};

inline constexpr std::array<psx::debug::VerbUsage, 7> kReplVerbs{{
    {"run <n>", "run n frames, then prompt again"},
    {"step [n]", "run n frames (default 1)"},
    {"newgame", "pulse Cross to the game prologue"},
    {"skip [n]", "pulse Start for n frames (default 500)"},
    {"quit | q", "detach the REPL and let the game run"},
    {"end", "end the run cleanly so run-end reports print"},
    {"help", "this list"},
}};

void replyToLog(std::string_view line) {
  lucent::info("repl", "{}", line);
}

} // namespace

bool Repl::readStdinLine(std::span<char> line) {
  return std::fgets(line.data(), static_cast<int>(line.size()), stdin) != nullptr;
}

bool Repl::beforeStep(Core *c, uint32_t step, LineReader readLine) {
  while (budget_ <= 0) {
    budget_ = read(c, step, readLine);
    if (budget_ < 0) {
      return false;
    }
  }
  --budget_;
  return true;
}

long Repl::read(Core *c, uint32_t f, LineReader readLine) {
  const psx::debug::ReplySink reply = &replyToLog;
  std::array<char, 256> line{};
  lucent::info("repl", "frame={} ready", f);
  while (true) {
    bool receivedLine = false;
    {
      // Waiting for input is intentional idle; resume before running a command so a stall is still caught.
      const WatchdogInputWait wait;
      receivedLine = readLine(line);
    }
    if (!receivedLine) {
      break;
    }
    const psx::debug::CommandArgs args(line.data());
    const std::string_view cmd = args.word(0);
    const std::optional<uint32_t> count = args.number(1);
    if (cmd.empty()) {
      continue;
    }
    if (cmd == "quit" || cmd == "q") {
      return -1;
    }
    if (cmd == "end") {
      return -2;
    }
    if (cmd == "run" && count.has_value()) {
      return static_cast<long>(*count);
    }
    if (cmd == "step") {
      return count.value_or(0) != 0 ? static_cast<long>(*count) : 1;
    }
    if (cmd == "newgame") {
      navNewgame = 1;
      lucent::info("repl", "newgame: pulsing to GAME prologue");
      return 100000;
    }
    if (cmd == "skip") {
      skipFrames = count.value_or(0) != 0 ? static_cast<long>(*count) : 500;
      lucent::info("repl", "skip {} frames", skipFrames);
      return skipFrames;
    }
    if (cmd == "help") {
      psx::debug::DebugCommands::help(kReplVerbs, reply);
      continue;
    }
    if (!c->game->debugCommands.run(*c, args, reply)) {
      lucent::info("repl", "? {}", cmd);
    }
  }
  return -1;
}

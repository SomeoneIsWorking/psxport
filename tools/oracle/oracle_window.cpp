#include "oracle_boundary.h"
#include "oracle_shim.h"
#include "oracle_snapshot.h"

#include <charconv>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

namespace {

bool number(const char *text, uint32_t &value) {
  std::string_view input(text);
  int base = 10;
  if (input.starts_with("0x")) {
    input.remove_prefix(2);
    base = 16;
  }
  const auto result = std::from_chars(input.data(), input.data() + input.size(), value, base);
  return result.ec == std::errc{} && result.ptr == input.data() + input.size();
}

void usage() {
  std::fprintf(
      stderr,
      "usage: oracle_window --load FILE --steps N --save FILE [--stop-at PC]\n"
      "   or: oracle_window --boundary REGISTERS --ram RAM --scratch SPAD --steps N --save FILE [--stop-at PC]\n"
      "Runs an isolated Mednafen CPU window; unsupported devices remain fatal boundaries.\n"
      "--steps 0 performs an exact snapshot roundtrip without executing instructions.\n");
}

} // namespace

int main(int argc, char **argv) {
  const char *load = nullptr, *save = nullptr, *boundary = nullptr, *ram = nullptr, *scratch = nullptr;
  uint32_t steps = 0, stop_at = 0;
  bool have_steps = false, have_stop = false;
  for (int i = 1; i < argc; ++i) {
    if (i + 1 >= argc) {
      usage();
      return 2;
    }
    const char *option = argv[i++];
    if (!std::strcmp(option, "--load") && !load) {
      load = argv[i];
    } else if (!std::strcmp(option, "--boundary") && !boundary) {
      boundary = argv[i];
    } else if (!std::strcmp(option, "--ram") && !ram) {
      ram = argv[i];
    } else if (!std::strcmp(option, "--scratch") && !scratch) {
      scratch = argv[i];
    } else if (!std::strcmp(option, "--save") && !save) {
      save = argv[i];
    } else if (!std::strcmp(option, "--steps") && !have_steps && number(argv[i], steps)) {
      have_steps = true;
    } else if (!std::strcmp(option, "--stop-at") && !have_stop && number(argv[i], stop_at) && !(stop_at & 3u)) {
      have_stop = true;
    } else {
      usage();
      return 2;
    }
  }
  const bool exact = load && !boundary && !ram && !scratch;
  const bool normalized = !load && boundary && ram && scratch;
  if ((!exact && !normalized) || !save || !have_steps || (load && !std::strcmp(load, save)) || (have_stop && !steps)) {
    usage();
    return 2;
  }
  if (!oracle_init()) {
    return 2;
  }
  if (!(exact ? oracle_snapshot_load(load) : oracle_boundary_import(boundary, ram, scratch))) {
    oracle_teardown();
    return 2;
  }
  OracleState state{};
  oracle_capture(&state);
  const uint32_t initial_pc = state.pc;
  uint32_t executed = 0;
  while (executed < steps && (!have_stop || state.pc != stop_at)) {
    const OracleStop stop = oracle_step();
    ++executed;
    oracle_capture(&state);
    if (stop != ORACLE_STOP_BUDGET) {
      break;
    }
  }
  const bool reached = !have_stop || state.pc == stop_at;
  const bool clean = state.stop == ORACLE_STOP_NONE || state.stop == ORACLE_STOP_BUDGET;
  const bool saved = oracle_snapshot_save(save);
  std::fprintf(stdout,
               "oracle window: attempted_steps=%u/%u pc=0x%08X->0x%08X cycles=%d stop=%s "
               "stop_address=0x%08X checkpoint=%s snapshot=%s\n",
               executed,
               steps,
               initial_pc,
               state.pc,
               state.timestamp,
               oracle_stop_name(state.stop),
               state.stop_addr,
               reached ? "reached" : "NOT REACHED",
               saved ? "saved" : "FAILED");
  oracle_teardown();
  return clean && reached && saved ? 0 : 1;
}

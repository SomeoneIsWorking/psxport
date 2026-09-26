// test_debug_server_port — PSXPORT_DEBUG_SERVER's value, interpreted once.
//
// THE BUG THIS PINS (measured 2026-09-26, Spyro 1, `scratch/live/live.log`):
//     [cfg]   PSXPORT_REPL = false [default]
//     [cfg:warn] UNKNOWN knob PSXPORT_DEBUG_SERVER is set and matched nothing — it did NOTHING in this run
// The knob was read through the legacy cfg_str() path and declared NOWHERE, so the environment audit
// named it UNKNOWN, DbgServer::start() saw nothing, no listener was ever bound, and every attempt to
// drive a running game over the live endpoint connected to a closed port. The audit reporting it is
// the reason this was findable at all; the fix is that it is a DECLARED knob whose text is read
// through one function.
//
// The second thing this pins is that ONE function owns the meaning. `DbgServer::start` and
// `native_boot`'s frame-cap lift both ask "is the endpoint on, and on which port", and when two
// readers answer that separately they can disagree — a run capped by one and unbound by the other
// exits before a client can drive it. So both call `debug_server_port`, and these cases are its
// contract: the sentinel, a real port, and every shape of text that must NOT bind a port.

#include "../runtime/psx/dbg_server.h"
#include "testutil.h"

#include <string>

// The default the sentinel resolves to. Named here rather than in the test body so a change to the
// default is a change to the contract, and the doc comment in dbg_server.h has to move with it.
static constexpr int kDefaultPort = 5959;

static void test_sentinel_and_real_ports(void) {
  // "1" is the documented sentinel for the default port, not port 1.
  CHECK_EQ(debug_server_port("1"), kDefaultPort);
  CHECK_EQ(debug_server_port("5959"), kDefaultPort);
  CHECK_EQ(debug_server_port("5971"), 5971);
  CHECK_EQ(debug_server_port("65535"), 65535);
}

static void test_everything_else_is_off(void) {
  // Off is the safe answer: a knob that silently binds a port nobody asked for is worse than one
  // that does not start, and each of these is text a stale script or a typo can produce.
  CHECK_EQ(debug_server_port(""), 0);
  CHECK_EQ(debug_server_port("0"), 0);
  CHECK_EQ(debug_server_port("true"), 0);
  CHECK_EQ(debug_server_port("yes"), 0);
  CHECK_EQ(debug_server_port("59 59"), 0);
  CHECK_EQ(debug_server_port("5959 "), 0); // trailing space: not a port
  CHECK_EQ(debug_server_port(" 5959"), 0); // leading space: not a port
  CHECK_EQ(debug_server_port("59a9"), 0);
  CHECK_EQ(debug_server_port("-1"), 0);
  CHECK_EQ(debug_server_port("99999"), 0); // out of range
  CHECK_EQ(debug_server_port("4294967296"), 0);
}

static void test_view_over_a_cvar_value(void) {
  // The knob is a TextVar, so the call site passes its std::string. The function takes a string_view,
  // which is what lets a temporary bind to it without a copy — and this case is here because a
  // signature change to std::string const& would compile and then dangle on that temporary.
  const std::string configured = "5971";
  CHECK_EQ(debug_server_port(configured), 5971);
  CHECK_EQ(debug_server_port(std::string("1")), kDefaultPort);
  CHECK_EQ(debug_server_port(std::string()), 0);
}

int main(void) {
  RUN(sentinel_and_real_ports);
  RUN(everything_else_is_off);
  RUN(view_over_a_cvar_value);
  return pt_summary();
}

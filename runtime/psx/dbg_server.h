// class DbgServer — the live, non-blocking TCP debug endpoint (127.0.0.1:<PSXPORT_DEBUG_SERVER>).
//
// One per Game (`c->game->dbg_server.method()`). The endpoint listens on a single host port and
// dispatches queued commands to the MAIN thread once per frame via `service()`. All the socket/
// thread machinery + pause/step state + held-input mask + main<->server handoff lives on the class.
//
// In SBS two Games each have their own DbgServer, but only one wins the host TCP port; the other's
// listener bind fails and its `mStarted` stays false. Callers with `Core* c` reach the endpoint via
// `c->game->dbg_server.method()`. No legacy free-function shims — all callers use the class directly.
#pragma once
#include "config_vars.h" // cv_debug_server — the endpoint's port
#include <cstddef>
#include <cstdint>
#include <pthread.h>
#include <string_view>
class Core;
class Game;

// The loopback port the live debug endpoint listens on, or 0 for "off".
//
// PSXPORT_DEBUG_SERVER carries a PORT, with 1 as the sentinel for the default, so it is read as text
// and interpreted here — once, for both readers. `native_boot` asks whether the endpoint is on (to
// lift the headless frame cap) and `DbgServer::start` asks which port to bind; when those two
// answered the knob separately they could disagree about whether it was set at all, which is how a
// capped run ended before a client could drive it.
//
// Unset, empty, "0" and anything that is not a number are OFF, because a knob that silently binds a
// port nobody asked for is worse than one that does not start.
inline int debug_server_port(std::string_view value) {
  int port = 0;
  for (const char c : value) {
    if (c < '0' || c > '9') {
      return 0; // a negative or non-numeric port is not a port; refuse rather than bind one
    }
    port = port * 10 + (c - '0');
    if (port > 65535) {
      return 0;
    }
  }
  if (port == 0) {
    return 0; // an empty value and an explicit "0" both land here, and both mean off
  }
  return port == 1 ? 5959 : port;
}

// Whether a client is going to drive this run over the live endpoint. Every boot spine asks this
// before applying its own frame cap: the cap exists to bound an unattended smoke run, and a run that
// is driven over the socket must not be capped, or the process exits before anyone can drive it.
// The conditions a particular spine adds (no REPL, no window) are that spine's policy and stay there.
inline bool debug_server_live() {
  return debug_server_port(psx::config::cv_debug_server.get()) != 0;
}

class DbgServer {
public:
  Game *game = nullptr; // back-pointer wired by Game()

  // Process entry — installed by boot when PSXPORT_DEBUG_SERVER names a port. NO-OP otherwise.
  void start(Core *c);

  // The once-per-frame half of a live session, called BEFORE the frame runs: while the client has
  // frozen the game, do not advance it — pump host input, re-show the last presented frame, and keep
  // servicing commands so `step` and `play` can arrive. One implementation for every boot spine,
  // because the framework's own loop and a title's own frame driver both have to honour a pause, and
  // two copies of "what a pause does" is how they come to disagree. The command itself is serviced
  // by `service()` AFTER the frame, so a read never observes a half-completed one.
  void honourPause(Core *c);

  // Called once per frame from the native frame loop. Services at most one queued command; the
  // Core* is stashed for the `call` subcommand to run guest fns at this frame boundary.
  void service(Core *c);

  // Pause / step gating polled by the frame loop.
  bool isPaused() const {
    return mPaused;
  }
  bool stepPending() const {
    return mStep > 0;
  }
  void consumeStep() {
    if (mStep > 0) {
      mStep--;
    }
  }
  void setPaused(bool p) {
    mPaused = p;
    mStep = 0;
  }
  void togglePause() {
    mPaused = !mPaused;
    mStep = 0;
  }
  void addStep(int n) {
    mPaused = true;
    mStep += n;
  }

  // Held-input mask (set by press/release/hold subcommands, applied to c->game->pad.driveHold).
  unsigned short heldMask() const {
    return mHeld;
  }
  void setHeldMask(unsigned short m) {
    mHeld = m;
  }

  // Command dispatcher's live Core pointer (set at the top of service()); nullptr outside a frame.
  Core *ctx() const {
    return mCtx;
  }

private:
  bool mPaused = false;
  int mStep = 0;
  unsigned short mHeld = 0xFFFF; // active-low held mask (all released)

  // Handoff between the TCP server thread and the MAIN thread. mMtx guards mReqPending/mRespReady/
  // mCmd/mRespBuf/mRespLen. See dbg_server.cpp for the timed-wait dance in dbg_submit.
  bool mStarted = false;
  Core *mCtx = nullptr;
  pthread_mutex_t mMtx = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t mDone = PTHREAD_COND_INITIALIZER; // signalled by main when a result is ready
  char mCmd[512] = {};                             // command awaiting service (server -> main)
  int mReqPending = 0;                             // 1 while a command is queued for the main thread
  int mRespReady = 0;                              // 1 once the main thread has produced a result
  char *mRespBuf = nullptr;                        // malloc'd result (main -> server); server frees after sending
  size_t mRespLen = 0;
  friend class DbgServerInternals; // dbg_server.cpp accessor helper (see impl file)
};

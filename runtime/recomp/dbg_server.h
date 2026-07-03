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
class Core;
class Game;

class DbgServer {
public:
  Game* game = nullptr;   // back-pointer wired by Game()

  // Process entry — installed by boot when PSXPORT_DEBUG_SERVER is set. NO-OP otherwise.
  void start(Core* c);

  // Called once per frame from the native frame loop. Services at most one queued command; the
  // Core* is stashed for the `call` subcommand to run guest fns at this frame boundary.
  void service(Core* c);

  // Pause / step gating polled by the frame loop.
  bool isPaused()      const { return mPaused; }
  bool stepPending()   const { return mStep > 0; }
  void consumeStep()          { if (mStep > 0) mStep--; }
  void setPaused(bool p)      { mPaused = p; mStep = 0; }
  void togglePause()          { mPaused = !mPaused; mStep = 0; }
  void addStep(int n)         { mPaused = true; mStep += n; }

  // Held-input mask (set by press/release/hold subcommands, applied to c->game->pad.driveHold).
  unsigned short heldMask() const   { return mHeld; }
  void setHeldMask(unsigned short m){ mHeld = m; }

  // Command dispatcher's live Core pointer (set at the top of service()); nullptr outside a frame.
  Core* ctx() const { return mCtx; }

private:
  bool  mPaused = false;
  int   mStep   = 0;
  unsigned short mHeld = 0xFFFF;   // active-low held mask (all released)

  // Handoff between the TCP server thread and the MAIN thread. mMtx guards mReqPending/mRespReady/
  // mCmd/mRespBuf/mRespLen. See dbg_server.cpp for the timed-wait dance in dbg_submit.
  bool  mStarted = false;
  Core* mCtx = nullptr;
  friend class DbgServerInternals;   // dbg_server.cpp accessor helper (see impl file)
};

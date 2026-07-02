// class DbgServer — the live, non-blocking TCP debug endpoint (127.0.0.1:<PSXPORT_DEBUG_SERVER>).
//
// One singleton per process — the debug server listens on a single host port and dispatches queued
// commands to the MAIN thread once per frame (dbg_server_service). All the socket/thread machinery
// + pause/step state + held-input mask + main<->server handoff live on the class; the command
// dispatcher (dbg_exec) is a file-scope helper in dbg_server.cpp that reads/writes those members.
//
// Not per-Core — process-scoped by design (single endpoint, single input feed).
//
// Legacy free-function API (dbg_is_paused / dbg_step_pending / dbg_consume_step / dbg_set_paused /
// dbg_toggle_pause / dbg_add_step / dbg_server_start / dbg_server_service) is kept for existing
// callers (fps60, native_boot, sbs, main loop) — each is a one-line bridge to the singleton method.
#pragma once
struct Core;

class DbgServer {
public:
  static DbgServer& instance();

  // Process entry — installed by boot when PSXPORT_DEBUG_SERVER is set. NO-OP otherwise.
  void start(Core* c);

  // Called once per frame from the native frame loop. Services at most one queued command; the
  // Core* is stashed for the `call` subcommand to run guest fns at this frame boundary. Pass
  // nullptr from sites without a context (call then reports "no context").
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
  DbgServer() = default;
  bool  mPaused = false;
  int   mStep   = 0;
  unsigned short mHeld = 0xFFFF;   // active-low held mask (all released)

  // Handoff between the TCP server thread and the MAIN thread. mMtx guards mReqPending/mRespReady/
  // mCmd/mRespBuf/mRespLen. See dbg_server.cpp for the timed-wait dance in dbg_submit.
  // (The pthread primitives are opaque to callers; declared as `void*` here to avoid pulling <pthread.h>
  // into the header. Actual mutex+cond instances live at file scope in dbg_server.cpp.)

  bool  mStarted = false;
  Core* mCtx = nullptr;
  friend class DbgServerInternals;   // dbg_server.cpp accessor helper (see impl file)
};

// Legacy free-function API — thin bridges to `DbgServer::instance()` methods.
void dbg_server_start  (Core* c);
void dbg_server_service(Core* c);
int  dbg_is_paused     (void);
int  dbg_step_pending  (void);
void dbg_consume_step  (void);
void dbg_set_paused    (int p);
void dbg_toggle_pause  (void);
void dbg_add_step      (int n);

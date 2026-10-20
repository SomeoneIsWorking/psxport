// The live loopback debug endpoint: a listener thread queues one command, service() runs it on the main thread.
#pragma once
#include "config_vars.h"
#include <atomic>
#include <string>
class Core;

namespace psx::debug {

// Whether a client drives this run over the live endpoint.
inline bool debugServerLive() {
  return psx::config::debug_server_port() != 0;
}

} // namespace psx::debug

class DbgServer {
public:
  DbgServer() = default;
  DbgServer(const DbgServer &) = delete;
  DbgServer &operator=(const DbgServer &) = delete;
  // Drops this Game's claim; the process-lifetime listener keeps running for the next claimant.
  ~DbgServer();

  // Starts the listener when PSXPORT_DEBUG_SERVER names a port and claims it for this Game.
  void start();
  // Takes the endpoint after the claiming Game was destroyed; false while another Game holds it.
  bool claimEndpoint();

  // Before a frame: while a client has paused the game, idle until `play` or `step` arrives.
  void honourPause(Core *c);
  // One idle tick of a held field: host input, repaint, one command, ~15 ms.
  void idleFrame(Core *c);
  // After a frame: runs at most one queued command.
  void service(Core *c);

  // `quit` from a client, taken once by the run that owns its end.
  bool takeQuitRequest() {
    return mQuitRequested.exchange(false);
  }
  void requestQuit() {
    mQuitRequested.store(true);
  }

  void togglePause() {
    mPaused = !mPaused;
    mStep = 0;
  }
  void addStep(int n) {
    mPaused = true;
    mStep += n;
  }

private:
  void runCommand(Core &core, const char *line, std::string &reply);

  bool mPaused = false;
  int mStep = 0;
  // Written by the listener thread, read by the product loop.
  std::atomic<bool> mQuitRequested{false};
};

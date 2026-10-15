#pragma once

namespace psx::host {

// A title's request to end its run, held per Game as `Game::run`. The run's length is RunBudget's;
// this is the title's own "done", read by TitleSession after the step that asked.
class SessionRun {
public:
  void requestEnd() {
    endRequested_ = true;
  }
  bool shouldEnd() const {
    return endRequested_;
  }

private:
  bool endRequested_ = false;
};

} // namespace psx::host

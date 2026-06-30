// coro.cpp — see coro.h. Strict ping-pong handoff between the scheduler thread and the fiber thread,
// arbitrated by `turn_` under one mutex/condvar. Exactly one side is ever runnable.
#include "coro.h"

void Coro::thread_main() {
  {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait(lk, [this] { return turn_ == Turn::Fiber; });   // block until the first resume()
  }
  // exit_now()/cancel longjmp here to unwind the fiber's guest call chain; setjmp returns non-zero then.
  if (setjmp(exit_jmp_) == 0)
    body_();                                                 // runs, ping-ponging via yield()
  {
    std::lock_guard<std::mutex> lk(m_);
    finished_ = true;
    turn_ = Turn::Scheduler;       // final handoff back to the waiting resume()/cancel()
  }
  cv_.notify_all();
}

void Coro::start(std::function<void()> body) {
  body_ = std::move(body);
  started_ = true;
  th_ = std::thread(&Coro::thread_main, this);
}

void Coro::resume() {
  if (!started_ || finished_) return;
  {
    std::lock_guard<std::mutex> lk(m_);
    turn_ = Turn::Fiber;
  }
  cv_.notify_all();
  std::unique_lock<std::mutex> lk(m_);
  cv_.wait(lk, [this] { return turn_ == Turn::Scheduler; });  // until the fiber yields or finishes
}

void Coro::yield() {
  {
    std::lock_guard<std::mutex> lk(m_);
    turn_ = Turn::Scheduler;
  }
  cv_.notify_all();
  {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait(lk, [this] { return turn_ == Turn::Fiber; });    // until the next resume()/cancel()
  }
  if (canceling_) longjmp(exit_jmp_, 1);   // cancel()'d while blocked -> unwind to the body root
}

void Coro::exit_now() {
  longjmp(exit_jmp_, 1);                    // fiber thread: unwind to thread_main's setjmp
}

void Coro::cancel() {
  if (!started_ || finished_) return;
  canceling_ = true;
  resume();   // wake the blocked fiber; yield() sees canceling_ and unwinds -> body finishes -> we return
}

Coro::~Coro() {
  // A live (blocked, not-finished) fiber MUST be cancel()'d before destruction — destroying the condvar
  // with a waiter is UB (hang/abort). cancel() drives it to finished_; then join cleanly.
  if (th_.joinable()) {
    if (!finished_) cancel();
    th_.join();
  }
}

// coro.h — a cooperative fiber implemented as a thread that BLOCKS on a condvar at a yield.
//
// WHY: the recompiled substrate (post-interpreter, later-254) can only ENTER a recompiled function at
// its top, so it cannot resume a cooperative PSX task at the mid-function PC where it yielded. A longjmp
// yield UNWINDS the C stack, destroying the resume point. A Coro instead runs the task body on its OWN
// thread and "yields" by BLOCKING that thread — the C stack (the whole nested recompiled call chain) is
// preserved, so resuming continues exactly where it left off, mid-function, recompiler-only. (USER
// directive 2026-06-30: "the PSX path needs to work recompiler-only; condvars with pause-resume.")
//
// It is a FIBER, not real concurrency: strict ping-pong, exactly one side runnable at a time. The
// scheduler calls resume() and blocks until the body calls yield() (still alive) or returns (finished).
// The body calls yield() and blocks until the next resume(). So guest register state (Core::r[], shared,
// single copy) is never touched by two parties at once — the scheduler save/restores it around the
// handoff exactly as the old longjmp scheduler did.
#pragma once
#include <condition_variable>
#include <csetjmp>
#include <functional>
#include <mutex>
#include <thread>

class Coro {
public:
  Coro() = default;
  ~Coro();
  Coro(const Coro&) = delete;
  Coro& operator=(const Coro&) = delete;

  // Spawn the fiber thread. It blocks immediately; nothing runs until the first resume(). `body` runs
  // to completion across resume()/yield() ping-pongs; when it returns the Coro is done().
  void start(std::function<void()> body);

  // SCHEDULER side: hand control to the fiber and block until it yields or finishes. No-op if done().
  void resume();

  // FIBER side (called from inside `body`, e.g. the yield override): hand control back to the scheduler
  // and block until the next resume(). Must only be called on this Coro's own thread. If the Coro was
  // cancel()'d while blocked, this UNWINDS the fiber (longjmp to the body root) instead of returning.
  void yield();

  // FIBER side: immediately unwind the fiber's (guest) call chain back to the body root so the Coro
  // finishes — for an in-body task END (the guest set state=0 and will never be resumed). The abandoned
  // frames are plain data, no C++ destructors to run. Never returns.
  [[noreturn]] void exit_now();

  // SCHEDULER side: tear down a fiber that is BLOCKED mid-yield (not done). Wakes it so yield() unwinds
  // to the body root and the Coro finishes — REQUIRED before delete, else ~Coro destroys a condvar with a
  // live waiter (UB / hang). No-op if not started or already done.
  void cancel();

  bool started() const { return started_; }
  bool done() const { return finished_; }

private:
  enum class Turn { Scheduler, Fiber };
  std::thread th_;
  std::mutex m_;
  std::condition_variable cv_;
  Turn turn_ = Turn::Scheduler;   // whose turn to run; the other side is blocked on cv_
  bool started_ = false;
  bool finished_ = false;
  bool canceling_ = false;        // set by cancel(); yield() unwinds instead of returning when seen
  std::jmp_buf exit_jmp_;         // body root, set in thread_main; exit_now()/cancel unwind here
  std::function<void()> body_;

  void thread_main();
};

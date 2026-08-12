// repl_service.cpp — the impure half of the "is anybody going to READ this stdin?" guard.
// The RULE lives in repl_service.h (pure, hermetically gated on both classes); this file only
// measures the process and reports. See that header for why a refusal, and not a best-effort.
#include "repl_service.h"

#include "config_vars.h"   // psx::config::cv_repl — PSXPORT_REPL through the CVar ladder

#include <lucent/log.h>

#include <cstdlib>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace psx {
namespace repl_service {

// Bytes ALREADY waiting on fd 0, or <0 for "cannot tell".
//
// It is deliberately NOT poll(POLLIN): poll reports a fd READABLE at end-of-file, so `< /dev/null`
// and a closed empty pipe — the two most common ways an agent launches a headless run — would both
// present as "a script is waiting" and every such run would be refused. A refusal that fires on
// correct usage is worse than the bug it guards, because after the second one nobody reads it.
long stdin_pending_bytes() {
  struct stat st;
  if (fstat(STDIN_FILENO, &st) != 0) return -1;
  if (S_ISREG(st.st_mode)) {
    // `binary < script.txt`: what is left between the offset and the end.
    const off_t off = lseek(STDIN_FILENO, 0, SEEK_CUR);
    if (off < 0) return -1;
    return st.st_size > off ? (long)(st.st_size - off) : 0;
  }
  if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode) || S_ISCHR(st.st_mode)) {
    int n = 0;
    if (ioctl(STDIN_FILENO, FIONREAD, &n) != 0) return -1;   // /dev/null answers 0, not an error
    return (long)n;
  }
  return -1;   // some other fd type: cannot tell, and "cannot tell" never refuses
}

void refuse_if_unserviced(const char* loopName, bool loopServicesRepl) {
  Query q;
  q.loopServicesRepl   = loopServicesRepl;
  q.replRequested      = psx::config::cv_repl.get();
  q.stdinIsTty         = isatty(STDIN_FILENO) != 0;
  q.stdinPendingBytes  = stdin_pending_bytes();

  const Verdict v = decide(q);
  if (v == Verdict::Ok) return;

  const std::string text = message(loopName, q, v);
  if (v == Verdict::Warn) {
    static bool warned = false;   // a keystroke per frame must not become a log firehose
    if (warned) return;
    warned = true;
    lucent::warn("repl", "{}", text);
    return;
  }
  // What the check could NOT see, printed WITH the refusal: this measures bytes already queued at
  // the moment of the call, so a driver that writes its first command later is caught by the
  // per-frame re-check, and a driver that writes NOTHING is indistinguishable from no driver.
  lucent::error("repl", "{} (probe: PSXPORT_REPL={}, stdin tty={}, pending bytes={} — this cannot "
                        "see a driver that has written nothing yet; the check re-runs every frame.)",
                text, q.replRequested ? 1 : 0, q.stdinIsTty ? 1 : 0, q.stdinPendingBytes);
  exit(kRefusalExit);
}

}  // namespace repl_service
}  // namespace psx

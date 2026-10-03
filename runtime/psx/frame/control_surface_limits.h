// control_surface_limits.h — the ONE home for limits that BOTH control surfaces obey.
//
// WHY A HEADER FOR ONE CONSTANT. `Repl` (stdin, PSXPORT_REPL=1) and `DbgServer` (the loopback live
// endpoint) are two transports over the same verbs, and until 2026-09-27 each carried its own literal
// `64` for the same limit. Two copies of a protocol constant is two places for the transports to drift,
// and a drift here is silent: one surface would answer 64 words while the other answered 128, and a
// tool that works against one would quietly mis-read the other.
//
// WHAT THE LIMIT IS. A read verb's answer must stay ONE LINE, because both transports are
// line-oriented and every consumer parses a single line. So a request for more words than this is
// served up to the cap and the REST IS NOT SERVED — which is the whole hazard, stated plainly:
//
//   A short answer that looks like a complete one is the worst thing a diagnostic can return, because
//   the caller has no way to tell it apart from a real reading of zeros.
//
// That is not hypothetical. Measured 2026-09-27 on Spyro 1: `tools/probe_moby_list.py` asked the live
// endpoint for 1408 words in one `rw` and the endpoint returned 64. The reader then waited for 1408
// words that were never coming and the probe hung until its timeout, having produced no output at all
// (its stdout was still buffered when it was killed). A less careful caller would have read the 64 words
// it got and treated the remaining 1344 as zero — which for that probe would have been reported as
// "the moby list is empty", the exact shape of the answer it was built to check.
//
// HOW A CLIENT SURVIVES THIS. Send one request per `kMaxControlReadWords`, then LOOP: re-request from
// `address + 4 * words_assembled` until the count you asked for is in hand. The cap becomes an iteration
// count instead of a ceiling. The reference implementation is
// `spider1/tools/probe_wide_geometry.py`; the wrong shape, for contrast, is `spyro/tools/drive.py`,
// which sends one request and then reports "the port exited before answering" when a short answer
// arrives — a message that sends the reader to debug the product instead of its own request. See
// `docs/findings/diagnostics-that-cannot-lie.md`.
//
// THE CONTRACT, for anyone adding a verb that reads a caller-specified span:
//   * serve at most kMaxControlReadWords words;
//   * if the request asked for more, SAY SO on its own line, naming how many were served and how many
//     were asked for, so the short answer cannot be read as a complete one;
//   * never pad the missing words with zeros, and never treat them as absent silently.
#pragma once

#include <cstdint>

namespace psx::control {

// Words (32-bit) one `rw`-style read may return on a single line. 64 words = 256 bytes, which is what a
// line-oriented protocol can carry without a reader having to reassemble.
inline constexpr std::uint32_t kMaxControlReadWords = 64;

} // namespace psx::control

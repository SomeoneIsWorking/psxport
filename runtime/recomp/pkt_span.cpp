// PktSpanSession (RAII object scope) implementation — separated from the header so the header can
// forward-declare Core*. See pkt_span.h for the class docs.
#include "pkt_span.h"
#include "core.h"          // Core::rsub (RenderSubstrate) — pkt_span lives on the framework substrate now

PktSpanSession::PktSpanSession(Core* c) : mPs(&c->rsub.pktSpan), mOuter(mPs->save()) {
  mPs->open();
}

int PktSpanSession::close(uint32_t* lo, uint32_t* hi) {
  uint32_t my_lo, my_hi;
  bool captured = mPs->current(&my_lo, &my_hi);
  mPs->restoreMerge(mOuter, captured ? my_lo : 0xFFFFFFFFu, captured ? my_hi : 0);
  if (captured) { if (lo) *lo = my_lo; if (hi) *hi = my_hi; return 1; }
  return 0;
}

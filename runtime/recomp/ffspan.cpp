// class FfSpan impl — see ffspan.h for the attribution model.
#include "core.h"
#include "cfg.h"
#include "ffspan.h"
#include <stdio.h>

int FfSpan::bdtagOn() {
  if (mBdTag < 0) mBdTag = cfg_str("PSXPORT_BDTAG") ? 1 : 0;
  return mBdTag;
}

const char* FfSpan::lookup(uint32_t a) const {
  for (int i = 0; i < mN; i++) if (a >= mSpans[i].lo && a < mSpans[i].hi) return mSpans[i].name;
  return "(unattributed)";
}

void FfSpan::resetFrame() { if (bdtagOn()) { mN = 0; mSp = 0; } }

void FfSpan::begin() {
  if (!bdtagOn() || mSp >= 8) return;
  PktSpan& ps = core->rsub.pktSpan;
  mStk[mSp++] = ps.save();
  ps.open();
}

void FfSpan::end(const char* nm) {
  if (!bdtagOn() || mSp <= 0) return;
  PktSpan& ps = core->rsub.pktSpan;
  uint32_t mlo, mhi;
  bool captured = ps.current(&mlo, &mhi);
  if (captured) record(nm, mlo, mhi);
  ps.restoreMerge(mStk[--mSp], captured ? mlo : 0xFFFFFFFFu, captured ? mhi : 0);
}

void FfSpan::dump(uint32_t a) {
  if (mDumped) return; mDumped = 1;
  cfg_logi("ffspan", "addr %08x NOT in any of %d field-frame spans:", a, mN);
  for (int i = 0; i < mN; i++)
    cfg_logi("ffspan", "  %-12s [%08x .. %08x)", mSpans[i].name, mSpans[i].lo, mSpans[i].hi);
}

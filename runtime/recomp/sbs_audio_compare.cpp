// sbs_audio_compare.cpp — SBS audio-oracle policy, separate from lockstep orchestration.
#include "sbs_audio_compare.h"

#include "audio_field_report.h"
#include "game.h"
#include <cstdlib>
#include <lucent/log.h>

void SbsAudioCompare::configure(bool oracleMode) {
  const char *e = getenv("PSXPORT_SBS_AUDIO");
  mEnabled = oracleMode || (e && *e && e[0] != '0');
  if (mEnabled) {
    lucent::info("sbs-audio",
                 "per-field audio oracle compare enabled (A reports vs B reports); no reports before SPU "
                 "advances are expected");
  }
}

void SbsAudioCompare::clear(Game *a, Game *b) {
  a->spu_audio.clearFieldReports();
  b->spu_audio.clearFieldReports();
}

void SbsAudioCompare::compare(Game *a, Game *b, uint32_t frame) {
  if (!mEnabled) {
    return;
  }
  const auto &aReports = a->spu_audio.fieldReports();
  const auto &bReports = b->spu_audio.fieldReports();
  if (aReports.empty() && bReports.empty()) {
    return;
  }
  const AudioFieldCompareResult result =
      compareAudioFieldReports(aReports.data(), aReports.size(), bReports.data(), bReports.size());
  if (!result.equal && !mMismatch) {
    mMismatch = true;
    lucent::error("sbs-audio", "f{}: {}", frame, result.reason);
  } else if (result.equal && !mReportedPass) {
    mReportedPass = true;
    lucent::info("sbs-audio", "f{}: compared {} exact PCM field reports with no mismatch", frame, aReports.size());
  }
}

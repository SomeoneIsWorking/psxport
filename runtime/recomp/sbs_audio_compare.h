// sbs_audio_compare.h — SBS policy for comparing each core's current audio field reports.
#pragma once

#include <cstdint>

class Game;

class SbsAudioCompare {
public:
  void configure(bool oracleMode);
  void clear(Game *a, Game *b);
  void compare(Game *a, Game *b, uint32_t frame);

private:
  bool mEnabled = false;
  bool mMismatch = false;
  bool mReportedPass = false;
};

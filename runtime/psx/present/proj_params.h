// proj_params.h — the camera projection as the game stated it through libgte SetGeomOffset/SetGeomScreen.
#pragma once
#include <cstdint>

class Core;

namespace psx::present {

// geomValid() stays false until both setters ran; the 0 defaults keep an unset projection observable.
class ProjParams {
public:
  void setGeomOffset(float ofx, float ofy) {
    mGeomOfx = ofx;
    mGeomOfy = ofy;
    mGeomOffsetSet = true;
  }
  void setGeomScreen(float h) {
    mGeomH = h;
    mGeomScreenSet = true;
  }
  bool geomValid() const {
    return mGeomOffsetSet && mGeomScreenSet;
  }
  float geomOfx() const {
    return mGeomOfx;
  }
  float geomOfy() const {
    return mGeomOfy;
  }
  float geomH() const {
    return mGeomH;
  }

private:
  float mGeomOfx = 0.0f;
  float mGeomOfy = 0.0f;
  float mGeomH = 0.0f;
  bool mGeomOffsetSet = false;
  bool mGeomScreenSet = false;
};

} // namespace psx::present

// The libgte leaves: the GTE control-register write and the recorded copy together, so they cannot drift.
namespace psx::gte {

void setGeomOffset(Core &core, std::int32_t ofx, std::int32_t ofy);
void setGeomScreen(Core &core, std::int32_t h);

} // namespace psx::gte

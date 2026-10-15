// field_cadence.h — display fields delivered in `parts` equal pacing steps.
#pragma once

namespace psx::frame {

struct FieldCadence {
  int fields = 0;
  int parts = 1;
};

} // namespace psx::frame

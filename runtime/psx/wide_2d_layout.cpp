#include "wide_2d_layout.h"

#include "core.h"
#include "game.h"
#include "gpu_vk.h"

int gpu_vk_wide_presentation(Core *);
int gpu_vk_wide_presentation_w(Core *);

namespace {

// The four facts, gathered from a Core. Everything the decision needs and nothing it does not.
struct Mechanism {
  int wide;
  bool engaged;
};

Mechanism host_mechanism(Core &core) {
  return Mechanism{gpu_vk_wide_engine(&core) ? gpu_vk_wide_engine_w(&core) : 0, gpu_vk_wide_engine(&core) != 0};
}

Mechanism guest_mechanism(Core &core) {
  return Mechanism{gpu_vk_wide_presentation(&core) ? gpu_vk_wide_presentation_w(&core) : 0,
                   gpu_vk_wide_presentation(&core) != 0};
}

} // namespace

Wide2dExtent wide_2d_extent(int host_wide, bool host_engaged, int guest_wide, bool guest_engaged, int native) {
  if (host_engaged) {
    return Wide2dExtent{host_wide, native};
  }
  if (guest_engaged) {
    return Wide2dExtent{guest_wide, native};
  }
  return Wide2dExtent{native, native};
}

bool wide_2d_layout_active_for(int host_wide, bool host_engaged, int guest_wide, bool guest_engaged, int native) {
  const Wide2dExtent extent = wide_2d_extent(host_wide, host_engaged, guest_wide, guest_engaged, native);
  return extent.wide > extent.native;
}

bool wide_2d_layout_active(Core &core) {
  const Mechanism host = host_mechanism(core);
  const Mechanism guest = guest_mechanism(core);
  return wide_2d_layout_active_for(host.wide, host.engaged, guest.wide, guest.engaged, gpu_vk_native_w(&core));
}

Rq2dXform wide_2d_layout(Core &core, Rq2dSpace space, int layer, bool flat, bool untextured) {
  const Mechanism host = host_mechanism(core);
  const Mechanism guest = guest_mechanism(core);
  const Wide2dExtent extent =
      wide_2d_extent(host.wide, host.engaged, guest.wide, guest.engaged, gpu_vk_native_w(&core));
  if (extent.wide <= extent.native) {
    return {}; // 4:3, or nothing widened: the identity, which is what the rule returns anyway
  }
  return rq_2d_xform(extent.wide, extent.native, space, layer, flat, untextured);
}

#include "picture_announce.h"

#include "core.h"
#include "game.h"
#include "gpu_vk.h"

#include <lucent/log.h>

namespace psx::picture {

void announceOnChange(Core &core) {
  const Geometry now{
      core.game->mods.aspect, gpu_vk_wide_engine(&core), (int)core.game->gpu.s_disp_w, gpu_vk_wide_engine_w(&core)};
  if (now == core.rsub.announcedPicture) {
    return;
  }
  core.rsub.announcedPicture = now;
  lucent::info("wide",
               "native picture: aspect={} wide_engine={} native_width={} render_width={}",
               now.aspect,
               now.wideEngine,
               now.nativeWidth,
               now.renderWidth);
}

} // namespace psx::picture

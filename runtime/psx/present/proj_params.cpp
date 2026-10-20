#include "proj_params.h"

#include "core.h"
#include "gte.h"
#include "gte_registers.h"

namespace psx::gte {

void setGeomOffset(Core &core, std::int32_t ofx, std::int32_t ofy) {
  // OFX/OFY are 16.16 fixed point.
  core.gte().writeControl(kOfx, static_cast<std::uint32_t>(ofx) << 16);
  core.gte().writeControl(kOfy, static_cast<std::uint32_t>(ofy) << 16);
  core.rsub.projParams.setGeomOffset(static_cast<float>(ofx), static_cast<float>(ofy));
}

void setGeomScreen(Core &core, std::int32_t h) {
  core.gte().writeControl(kH, static_cast<std::uint32_t>(h));
  core.rsub.projParams.setGeomScreen(static_cast<float>(h));
}

} // namespace psx::gte

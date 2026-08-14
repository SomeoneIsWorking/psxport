#pragma once

#include "native_projection.h"

namespace psxport::native_projection::detail {

// Compatibility adapter for the framework's RTPS/RTPT diagnostic probe. This
// is deliberately not part of the producer-facing API.
NativeProjectedVertex project_gte_mode(const FixedAffine &affine,
                                       const ProjectionParams &projection,
                                       ModelVertex vertex, unsigned shift,
                                       bool limit_mode);

} // namespace psxport::native_projection::detail

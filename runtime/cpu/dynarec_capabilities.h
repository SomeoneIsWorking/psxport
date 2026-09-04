#pragma once

#define PSXPORT_HAS_DYNAREC_RUNTIME 1
#define PSXPORT_HAS_LIGHTREC_BACKEND 1
#define PSXPORT_HAS_BOUNDED_INTERPRETER_FALLBACK 1
#define PSXPORT_HAS_LIGHTREC_AARCH64 0

namespace psx::cpu {

struct DynarecBackendCapabilities {
  bool available;
  bool dynarecDefault;
  bool boundedInterpreterFallback;
  bool fallbackTelemetry;
  bool fallbackThresholdEnforcement;
  bool aarch64CodeGeneration;
  bool executableMemoryPublication;
  bool instructionCacheCoherence;
  bool rangeInvalidation;
  bool hostAbiTransitions;
};

inline constexpr DynarecBackendCapabilities kLightrecBackendCapabilities{
    .available = true,
    .dynarecDefault = true,
    .boundedInterpreterFallback = true,
    .fallbackTelemetry = true,
    .fallbackThresholdEnforcement = false,
    .aarch64CodeGeneration = false,
    .executableMemoryPublication = true,
    .instructionCacheCoherence = true,
    .rangeInvalidation = true,
    .hostAbiTransitions = true,
};

} // namespace psx::cpu

// PSXPORT_* knobs: lucent CVars, registered by the translation unit that defines them.
#pragma once

#include <lucent/cvar.hpp>

#include <functional>
#include <initializer_list>

namespace psx::config {

using lucent::cvar::Layer;
using lucent::cvar::Var;

inline const lucent::cvar::Options kRunOnly{lucent::cvar::Persistence::RunOnly, {}};
inline const lucent::cvar::Options kSaved{lucent::cvar::Persistence::Saved, {}};

// Registers one translation unit's knobs; defined right after them so none is read unregistered.
class Registration {
public:
  Registration(std::initializer_list<std::reference_wrapper<lucent::cvar::VarBase>> vars);
};

} // namespace psx::config

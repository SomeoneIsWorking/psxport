#include "mod_row_model.h"

#include "mods.h"

#include <cstdio>

namespace psx::ui {
namespace {

// ---- toggle rows ---------------------------------------------------------------------------------
// `states` is the cycle length: `v = (v + 1) % states`. That reproduces the three hand-written
// cycles it replaces exactly — `(aspect + 1) & 3` for 4 states, `ires += 1; if (ires > 4) ires = 0`
// for 5, and `!v` for a 0/1 bool.
//
// `fallback` is the value DISPLAYED for an out-of-range field, and it is deliberately per-row
// rather than a uniform 0: the old code clamped a bad `aspect` to 0 (Vanilla) but a bad `ires` to 1
// (also Vanilla, but index 1 because 0 means Auto for that row). Collapsing the two to one constant
// would have silently changed what a corrupt settings file shows.
struct ToggleRow {
  const char *id;
  int *(*field)(Mods &);
  const char *const *labels;
  int states;
  int fallback;
  bool persist; // debug toggles are deliberately not written to the settings file
};

const char *const kOnOff[] = {"Off", "On"};
const char *const kAspect[] = {"Vanilla", "16:9", "21:9", "Auto"};
// Merged resolution selector: 0 = Auto, 1..4 = Vanilla(1x)/X2/X3/X4.
const char *const kIres[] = {"Auto", "Vanilla", "X2", "X3", "X4"};
// Named for what the player sees, not for the mechanism: "Depth" is per-pixel depth ordering (this
// port's improvement), "Authored" is the order the game itself filed its polygons in.
const char *const kFaceOrder[] = {"Depth (enhanced)", "Authored (faithful)"};

const ToggleRow kToggleRows[] = {
    {"aspect",
     [](Mods &m) {
       return &m.aspect;
     },
     kAspect,
     4,
     0,
     true},
    {"ires",
     [](Mods &m) {
       return &m.ires;
     },
     kIres,
     5,
     1,
     true},
    {"face_order",
     [](Mods &m) {
       return &m.face_order;
     },
     kFaceOrder,
     2,
     0,
     true},
    {"fps60",
     [](Mods &m) {
       return &m.fps60;
     },
     kOnOff,
     2,
     0,
     true},
    {"ssao",
     [](Mods &m) {
       return &m.ssao;
     },
     kOnOff,
     2,
     0,
     true},
    {"light",
     [](Mods &m) {
       return &m.light;
     },
     kOnOff,
     2,
     0,
     true},
    {"shadows",
     [](Mods &m) {
       return &m.shadows;
     },
     kOnOff,
     2,
     0,
     true},
    {"debug_ids",
     [](Mods &m) {
       return &m.debug_ids;
     },
     kOnOff,
     2,
     0,
     false},
    {"debug_quads",
     [](Mods &m) {
       return &m.debug_quads;
     },
     kOnOff,
     2,
     0,
     false},
    {"debug_objects",
     [](Mods &m) {
       return &m.debug_objects;
     },
     kOnOff,
     2,
     0,
     false},
};

// ---- adjust rows ----------------------------------------------------------------------------------
struct AdjustRow {
  const char *id;
  float *(*field)(Mods &);
  float step, lo, hi;
  int precision;
};

const AdjustRow kAdjustRows[] = {
    {"ssao_strength",
     [](Mods &m) {
       return &m.ssao_strength;
     },
     0.05f,
     0.0f,
     2.0f,
     2},
    {"ssao_radius",
     [](Mods &m) {
       return &m.ssao_radius;
     },
     0.5f,
     1.0f,
     20.0f,
     1},
    {"ssao_bias",
     [](Mods &m) {
       return &m.ssao_bias;
     },
     0.002f,
     0.0f,
     0.1f,
     3},
    {"ssao_range",
     [](Mods &m) {
       return &m.ssao_range;
     },
     0.01f,
     0.02f,
     0.6f,
     3},
    {"light_dir_x",
     [](Mods &m) {
       return &m.light_dir[0];
     },
     0.05f,
     -1.0f,
     1.0f,
     2},
    {"light_dir_y",
     [](Mods &m) {
       return &m.light_dir[1];
     },
     0.05f,
     -1.0f,
     1.0f,
     2},
    {"light_dir_z",
     [](Mods &m) {
       return &m.light_dir[2];
     },
     0.05f,
     -1.0f,
     1.0f,
     2},
    {"light_ambient",
     [](Mods &m) {
       return &m.light_ambient;
     },
     0.05f,
     0.0f,
     1.5f,
     2},
    {"light_diffuse",
     [](Mods &m) {
       return &m.light_diffuse;
     },
     0.05f,
     0.0f,
     1.5f,
     2},
    {"shadow_strength",
     [](Mods &m) {
       return &m.shadow_strength;
     },
     0.05f,
     0.0f,
     1.0f,
     2},
};

const ToggleRow *find_toggle(std::string_view id) {
  for (const ToggleRow &r : kToggleRows) {
    if (id == r.id) {
      return &r;
    }
  }
  return nullptr;
}

const AdjustRow *find_adjust(std::string_view id) {
  for (const AdjustRow &r : kAdjustRows) {
    if (id == r.id) {
      return &r;
    }
  }
  return nullptr;
}

std::string fmt_f(float v, int precision) {
  char b[32];
  snprintf(b, sizeof b, "%.*f", precision, v);
  return b;
}

} // namespace

bool ModRowModel::knows(RowKind kind, std::string_view id) {
  if (kind == RowKind::Toggle) {
    return find_toggle(id) != nullptr;
  }
  if (kind == RowKind::Adjust) {
    return find_adjust(id) != nullptr;
  }
  return false;
}

bool ModRowModel::available(const Mods &m, RowKind kind, std::string_view id) {
  return kind != RowKind::Toggle || id != "fps60" || m.temporalInterpolationSupported();
}

bool ModRowModel::value_text(const Mods &m, RowKind kind, std::string_view id, std::string &out) {
  // The tables' accessors take a non-const Mods& because they are shared with the mutating paths;
  // reading through them here is const in effect, and casting once at the single read site is
  // clearer than maintaining a parallel const table.
  Mods &mm = const_cast<Mods &>(m);
  if (kind == RowKind::Toggle) {
    const ToggleRow *r = find_toggle(id);
    if (!r) {
      return false;
    }
    int v = *r->field(mm);
    if (v < 0 || v >= r->states) {
      v = r->fallback;
    }
    out = r->labels[v];
    return true;
  }
  if (kind == RowKind::Adjust) {
    const AdjustRow *r = find_adjust(id);
    if (!r) {
      return false;
    }
    out = fmt_f(*r->field(mm), r->precision);
    return true;
  }
  return false;
}

void ModRowModel::toggle(Mods &m, std::string_view id) {
  if (!available(m, RowKind::Toggle, id)) {
    return;
  }
  const ToggleRow *r = find_toggle(id);
  if (!r) {
    return;
  }
  int *v = r->field(m);
  *v = (*v + 1) % r->states;
  if (r->persist) {
    m.save();
  }
}

void ModRowModel::adjust(Mods &m, std::string_view id, int dir) {
  const AdjustRow *r = find_adjust(id);
  if (!r) {
    return;
  }
  float *v = r->field(m);
  *v += r->step * (float)dir;
  if (*v < r->lo) {
    *v = r->lo;
  }
  if (*v > r->hi) {
    *v = r->hi;
  }
  m.save();
}

int ModRowModel::toggle_count() {
  return (int)(sizeof(kToggleRows) / sizeof(kToggleRows[0]));
}
int ModRowModel::adjust_count() {
  return (int)(sizeof(kAdjustRows) / sizeof(kAdjustRows[0]));
}

} // namespace psx::ui

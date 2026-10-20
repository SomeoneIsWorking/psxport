#include "mod_row_model.h"

#include "mods.h"

namespace psx::ui {
namespace {

// `fallback` is the label index shown for an out-of-range field.
struct ToggleRow {
  const char *id;
  int *(*field)(Mods &);
  const char *const *labels;
  int states;
  int fallback;
};

const char *const kOnOff[] = {"Off", "On"};
const char *const kAspect[] = {"Vanilla", "16:9", "21:9", "Auto"};
// 0 = Auto, 1..4 = Vanilla(1x)/X2/X3/X4.
const char *const kIres[] = {"Auto", "Vanilla", "X2", "X3", "X4"};

const ToggleRow kToggleRows[] = {
    {"aspect",
     [](Mods &m) {
       return &m.aspect;
     },
     kAspect,
     4,
     0},
    {"ires",
     [](Mods &m) {
       return &m.ires;
     },
     kIres,
     5,
     1},
    {"fps60",
     [](Mods &m) {
       return &m.fps60;
     },
     kOnOff,
     2,
     0},
};

const ToggleRow *find_toggle(std::string_view id) {
  for (const ToggleRow &r : kToggleRows) {
    if (id == r.id) {
      return &r;
    }
  }
  return nullptr;
}

} // namespace

bool ModRowModel::knows(std::string_view id) {
  return find_toggle(id) != nullptr;
}

bool ModRowModel::available(const Mods &m, std::string_view id) {
  return id != "fps60" || m.temporalInterpolationSupported();
}

bool ModRowModel::value_text(const Mods &m, std::string_view id, std::string &out) {
  const ToggleRow *r = find_toggle(id);
  if (!r) {
    return false;
  }
  // The table's accessors are shared with toggle(); this read does not write.
  int v = *r->field(const_cast<Mods &>(m));
  if (v < 0 || v >= r->states) {
    v = r->fallback;
  }
  out = r->labels[v];
  return true;
}

void ModRowModel::toggle(Mods &m, std::string_view id) {
  if (!available(m, id)) {
    return;
  }
  const ToggleRow *r = find_toggle(id);
  if (!r) {
    return;
  }
  int *v = r->field(m);
  *v = (*v + 1) % r->states;
  m.save();
}

int ModRowModel::toggle_count() {
  return static_cast<int>(sizeof(kToggleRows) / sizeof(kToggleRows[0]));
}

} // namespace psx::ui

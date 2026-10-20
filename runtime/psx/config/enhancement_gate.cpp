#include "enhancement_gate.h"

#include "diagnostic_run.h"
#include "number_text.h"

#include <lucent/log.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace psx::config {
namespace {

Var<std::string> cv_enh("PSXPORT_ENH", "", kSaved);
const Registration kRegistration{cv_enh};

// Announcements are keyed by knob and outcome, and the PSXPORT_ENH parse is cached against its text.
struct EnhState {
  std::mutex mutex;
  std::vector<std::string> warnedSuppressed;
  std::vector<std::string> warnedInvalidMode;
  std::vector<std::string> warnedActive;
  bool warnedNameless = false;
  std::string parsedText = "\x01";
  bool parsedAll = false;
  std::vector<std::string> parsedNames;
};

constinit EnhState *enhancementState = nullptr;

EnhState &enhState() {
  if (enhancementState == nullptr) {
    enhancementState = new EnhState;
  }
  return *enhancementState;
}

bool contains(const std::vector<std::string> &names, std::string_view name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

// True the first time `key` is seen in `seen`. Caller holds the lock.
bool firstTime(std::vector<std::string> &seen, const char *key) {
  if (contains(seen, key)) {
    return false;
  }
  seen.emplace_back(key);
  return true;
}

} // namespace

bool enh_gate(const char *key, bool asked) {
  EnhState &s = enhState();
  if (key == nullptr || *key == '\0') {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.warnedNameless) {
      s.warnedNameless = true;
      lucent::error("cfg", "enhancement gate called with an empty name; refused");
    }
    return false;
  }
  const DiagnosticRunMode runMode = diagnostic_run_mode();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (asked && diagnostic_run_is_comparison(runMode)) {
    if (firstTime(s.warnedSuppressed, key)) {
      lucent::warn("cfg",
                   "{} enhancement suppressed: diagnostic run '{}' must remain enhancement-free",
                   key,
                   diagnostic_run_mode_name(runMode));
    }
    return false;
  }
  if (asked && runMode == DiagnosticRunMode::Invalid) {
    if (firstTime(s.warnedInvalidMode, key)) {
      lucent::error("cfg",
                    "{} enhancement refused: PSXPORT_DIAGNOSTIC_RUN is invalid; expected product, "
                    "compare-candidate, or compare-reference",
                    key);
    }
    return false;
  }
  // A run with an enhancement on names it, so a byte-compare against it is not read as a port bug.
  if (asked && firstTime(s.warnedActive, key)) {
    lucent::info("cfg", "{} enhancement active in product run", key);
  }
  return asked;
}

bool enh(const Var<bool> &v) {
  return enh_gate(v.name().c_str(), v.get());
}

long enh_int(const Var<long> &v) {
  const long value = v.get();
  return enh_gate(v.name().c_str(), value != 0) ? value : 0;
}

bool enh_named(const char *name) {
  if (name == nullptr || *name == '\0') {
    return enh_gate(name, false);
  }
  const std::string &text = cv_enh.get();
  bool selected = false;
  {
    EnhState &s = enhState();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.parsedText != text) {
      s.parsedText = text;
      s.parsedAll = text == "all";
      s.parsedNames.clear();
      if (!s.parsedAll) {
        for (const std::string_view token : splitList(text, ",: ")) {
          s.parsedNames.emplace_back(token);
        }
      }
    }
    selected = s.parsedAll || contains(s.parsedNames, name);
  }
  return enh_gate(name, selected);
}

} // namespace psx::config

// config.cpp — the CVar registry, the environment binding, the inventory, and the audit.
//
// Everything lives in one translation unit on purpose. The registry, the knob definitions and the
// audit are pulled into any link that touches ANY of them, so it is not possible to end up with a
// binary that has the lookup but not the knobs, or the knobs but not the reporting. psxport has
// already paid once for an initialisation step that was reachable only from one entry point
// (PSXPORT_DEBUG, see tests/test_lucent_channel_env.cpp); one object file with no ordering
// requirements is the cheapest way not to repeat it.
#include "config.h"
#include "config_vars.h"

#include <lucent/config.h>
#include <lucent/log.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>

namespace psx::config {

namespace {

// The registry. A function-local static, so it is constructed by the first CVar constructor that
// touches it and cannot be a victim of static-initialisation order.
struct Registry {
  std::mutex mutex;
  std::map<std::string, CVarBase*> vars;
  // Knobs that no CVar declares but which some cfg_* call has READ this run, with what they
  // resolved to. This is the compatibility path's visibility: an un-migrated knob is invisible until
  // something reads it, which is honest and is why the audit reports it as its own class.
  struct LegacyRead {
    Kind kind;
    std::string resolved;
  };
  std::map<std::string, LegacyRead> legacy;
};

Registry& reg() {
  static Registry r;
  return r;
}

// The one place that decides what counts as one of our knobs. lucent::config::active() lists the
// whole environment when the prefix is empty (psxport never calls set_prefix — every call site
// passes the full PSXPORT_-prefixed name), so the filter lives here.
bool is_psxport_name(std::string_view s) { return s.rfind("PSXPORT_", 0) == 0; }

std::string name_of_entry(const std::string& entry) {
  const std::size_t eq = entry.find('=');
  return eq == std::string::npos ? entry : entry.substr(0, eq);
}

bool contains(const std::vector<std::string>& v, std::string_view s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

// ── names ───────────────────────────────────────────────────────────────────────────────────────

const char* layer_name(Layer l) {
  switch (l) {
    case Layer::Default: return "default";
    case Layer::Value: return "value";       // psxport_settings.ini
    case Layer::Override: return "env";      // PSXPORT_* launch environment
    case Layer::Runtime: return "runtime";   // REPL / debug-server, this run only
  }
  return "?";
}

const char* kind_name(Kind k) {
  switch (k) {
    case Kind::Bool: return "bool";
    case Kind::Int: return "int";
    case Kind::Text: return "text";
  }
  return "?";
}

// ── detail: the forwarders and the registry hooks ───────────────────────────────────────────────

namespace detail {

// These four are the ENTIRE environment-reading surface of the CVar system, and each is exactly the
// expression the pre-migration cfg_* function used. That is what makes the compatibility claim
// structural rather than aspirational.
bool env_present(const char* name) { return lucent::config::present(name); }
bool env_bool(const char* name) { return lucent::config::flag(name); }
long env_int(const char* name, long fallback) { return lucent::config::number(name, fallback); }
std::string env_text(const char* name) { return lucent::config::text(name); }

void registry_lock() { reg().mutex.lock(); }
void registry_unlock() { reg().mutex.unlock(); }

void registry_add(CVarBase* v) {
  Registry& r = reg();
  std::lock_guard<std::mutex> lock(r.mutex);
  const auto it = r.vars.find(v->name());
  if (it != r.vars.end()) {
    // Two CVars claiming one name is a build-time mistake that would make `find()` return whichever
    // object won a race. Say so; do not silently keep one.
    lucent::error("cfg", "CVar name collision: {} is declared twice — the second declaration is ignored",
                  v->name());
    return;
  }
  r.vars.emplace(v->name(), v);
}

void registry_remove(CVarBase* v) {
  Registry& r = reg();
  std::lock_guard<std::mutex> lock(r.mutex);
  const auto it = r.vars.find(v->name());
  if (it != r.vars.end() && it->second == v) r.vars.erase(it);
}

}  // namespace detail

// ── CVarBase ────────────────────────────────────────────────────────────────────────────────────

CVarBase::CVarBase(const char* name, Kind kind, const char* help, bool persistable, bool external)
    : mName(name), mHelp(help), mKind(kind), mPersistable(persistable), mExternal(external) {
  detail::registry_add(this);
}

CVarBase::~CVarBase() { detail::registry_remove(this); }

Layer CVarBase::layer() const {
  ensure_env_bound();
  for (int i = kLayerCount - 1; i >= 1; --i)
    if (mSet[i]) return (Layer)i;
  return Layer::Default;
}

bool CVarBase::has(Layer l) const {
  ensure_env_bound();
  return mSet[(int)l];
}

void CVarBase::ensure_env_bound() const {
  if (mEnvBound) return;
  // Set the flag BEFORE binding: bind_env() reads the environment, which can log, and a logging call
  // that came back here would recurse. Binding twice is harmless; recursing is not.
  mEnvBound = true;
  const_cast<CVarBase*>(this)->bind_env();
}

// ── CVar<T> ─────────────────────────────────────────────────────────────────────────────────────

template <> std::string CVar<bool>::to_text(const bool& v) { return v ? "true" : "false"; }
template <> std::string CVar<long>::to_text(const long& v) { return std::to_string(v); }
template <> std::string CVar<std::string>::to_text(const std::string& v) { return v; }

template <>
bool CVar<bool>::set_text(Layer l, std::string_view text) {
  std::string s(text);
  for (char& c : s) c = (char)tolower((unsigned char)c);
  // Deliberately the same vocabulary lucent::config::flag accepts, so a value that means "off" in
  // the environment means "off" in the settings file and in a REPL command too.
  const bool off = s == "0" || s == "false" || s == "no" || s == "off";
  const bool on = s == "1" || s == "true" || s == "yes" || s == "on";
  if (!off && !on) return false;
  set(l, on);
  return true;
}

template <>
bool CVar<long>::set_text(Layer l, std::string_view text) {
  const std::string s(text);
  char* end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 0);
  if (s.empty() || !end || *end != '\0') return false;
  set(l, v);
  return true;
}

template <>
bool CVar<std::string>::set_text(Layer l, std::string_view text) {
  set(l, std::string(text));
  return true;
}

// The environment binding, per Kind.
//
// `external` CVars (PSXPORT_DEBUG, PSXPORT_LOG_FILE) bind their Override layer like any other. That
// is not a shadow copy that can drift: lucent reads the environment variable OF THAT NAME, built in
// as LUCENT_CHANNEL_ENV / LUCENT_LOG_FILE_ENV, so both of us are reading the same string. What
// `external` refuses is MUTATION — set_from_settings_file / set_runtime will not touch them, because
// writing here would not change what lucent does and would then be a lie on the dump. (The one way
// this could drift is a call to lucent::set_channel_env() renaming the variable at runtime; nothing
// in psxport does that, and the build-time name is what makes it unnecessary.)
template <>
void CVar<bool>::bind_env() {
  if (!detail::env_present(mName)) return;
  mSlot[(int)Layer::Override - 1] = detail::env_bool(mName);
  mSet[(int)Layer::Override] = true;
}

template <>
void CVar<long>::bind_env() {
  if (!detail::env_present(mName)) return;
  // The VALUE is whatever the pre-migration call would have produced — lucent::config::number with
  // this CVar's default as the fallback — so migrating cannot change what the run does. The warning
  // is the part that is new: a typo'd integer used to silently become the default, and a knob that
  // silently did not apply is the exact failure this system exists to make impossible.
  const std::string raw = detail::env_text(mName);
  char* end = nullptr;
  (void)std::strtol(raw.c_str(), &end, 0);
  if (raw.empty() || !end || *end != '\0')
    lucent::warn("cfg", "{}=\"{}\" is not an integer — falling back to the default {}", mName, raw,
                 mDefault);
  mSlot[(int)Layer::Override - 1] = detail::env_int(mName, mDefault);
  mSet[(int)Layer::Override] = true;
}

template <>
void CVar<std::string>::bind_env() {
  if (!detail::env_present(mName)) return;
  mSlot[(int)Layer::Override - 1] = detail::env_text(mName);
  mSet[(int)Layer::Override] = true;
}

template class CVar<bool>;
template class CVar<long>;
template class CVar<std::string>;

// ── THE INVENTORY ───────────────────────────────────────────────────────────────────────────────
// Declared in config_vars.h, which carries the per-knob documentation. Keep the two in the same
// order.
BoolVar cv_oracle("PSXPORT_ORACLE", false, "pure PSX reference mode: no native render enhancement",
                  /*persistable=*/false);
BoolVar cv_noaudio("PSXPORT_NOAUDIO", false, "open no audio device", /*persistable=*/false);
BoolVar cv_nopace("PSXPORT_NOPACE", false, "do not sleep to hold the present rate",
                  /*persistable=*/false);
BoolVar cv_repl("PSXPORT_REPL", false,
                "interactive REPL on stdin — SERVICED ONLY by the single-core native frame loop; "
                "REFUSED (exit 2) under the SBS harness (repl_service.h)",
                /*persistable=*/false);

IntVar cv_watchdog("PSXPORT_WATCHDOG", 3, "frame-progress timeout, seconds (0 = off)",
                   /*persistable=*/false);
IntVar cv_watchdog_boot("PSXPORT_WATCHDOG_BOOT", -1,
                        "first-present grace, seconds (-1 = derive: max(WATCHDOG, 45))",
                        /*persistable=*/false);

TextVar cv_asset_dir("PSXPORT_ASSET_DIR", "", "directory containing assets/ (empty = cwd-relative)",
                     /*persistable=*/false);
TextVar cv_settings_path("PSXPORT_SETTINGS", "psxport_settings.ini",
                         "path to the settings file — selects the Value layer, so never persisted",
                         /*persistable=*/false);

TextVar cv_producers_dir("PSXPORT_PRODUCERS_DIR", "scratch/producers",
                         "where the producer-census JSONL + accumulated claim set are written",
                         /*persistable=*/false);
TextVar cv_producers_db("PSXPORT_PRODUCERS_DB", "",
                        "claim set the guest leg resolves against (flat claims file or a run JSONL); "
                        "empty = <PRODUCERS_DIR>/claims.txt",
                        /*persistable=*/false);

BoolVar cv_fps60("PSXPORT_FPS60", false, "interpolated-60fps tier (Value layer = fps60= in the settings file)");

// PSXPORT_RENDER_PATH — the RENDER PATH tri-state: native | gte | psx
// (docs/plans/render-path-tristate.md). A TextVar rather than an int so the settings file and the REPL
// both read as the thing they select, and so a typo is REJECTED by render_path_parse instead of
// resolving to a plausible number. Persistable: the F1 overlay writes the Value layer, like fps60.
TextVar cv_render_path("PSXPORT_RENDER_PATH", "native",
                       "render path: native (PC producers) | gte (guest GTE+OT on the PC rasterizer) | "
                       "psx (guest GTE+OT on the PSX software rasterizer). The two guest paths are PURE — "
                       "fps60/wide/ires/deferred are native-only.");

// render_path() — the resolved path, with a NAMED refusal rather than a silent default. A value that
// parses to nothing is a knob that did nothing, and the CVar audit's whole purpose is that such a knob
// says so out loud (docs/config.md).
RenderPath render_path() {
  const std::string s = cv_render_path.get();
  RenderPath p = RenderPath::Native;
  if (!render_path_parse(s.c_str(), &p)) {
    lucent::warn("cfg", "PSXPORT_RENDER_PATH='{}' matched NO render path — falling back to 'native'. "
                        "Valid: native | gte | psx.", s);
    return RenderPath::Native;
  }
  return p;
}

TextVar cv_debug_channels("PSXPORT_DEBUG", "", "enabled diagnostic channels — READ BY LUCENT, declared here for visibility",
                          /*persistable=*/false, /*external=*/true);
TextVar cv_log_file("PSXPORT_LOG_FILE", "", "log output path — READ BY LUCENT, declared here for visibility",
                    /*persistable=*/false, /*external=*/true);

BoolVar cv_selftest_declared("PSXPORT_CFG_SELFTEST_DECLARED", false,
                             "reserved: the environment audit's calibration target, configures nothing",
                             /*persistable=*/false);

// ── lookup ──────────────────────────────────────────────────────────────────────────────────────

CVarBase* find(std::string_view name) {
  Registry& r = reg();
  std::lock_guard<std::mutex> lock(r.mutex);
  const auto it = r.vars.find(std::string(name));
  return it == r.vars.end() ? nullptr : it->second;
}

void enumerate(const std::function<void(CVarBase&)>& fn) {
  // Copy under the lock, then call: a callback that reads a CVar takes the lock itself.
  std::vector<CVarBase*> snapshot;
  {
    Registry& r = reg();
    std::lock_guard<std::mutex> lock(r.mutex);
    for (const auto& kv : r.vars) snapshot.push_back(kv.second);
  }
  for (CVarBase* v : snapshot) fn(*v);
}

std::size_t registered_count() {
  Registry& r = reg();
  std::lock_guard<std::mutex> lock(r.mutex);
  return r.vars.size();
}

// ── the legacy compatibility path ───────────────────────────────────────────────────────────────

void note_legacy_read(const char* name, Kind kind, std::string_view resolved) {
  if (!name || !is_psxport_name(name)) return;
  Registry& r = reg();
  std::lock_guard<std::mutex> lock(r.mutex);
  r.legacy[name] = Registry::LegacyRead{kind, std::string(resolved)};
}

std::size_t legacy_read_count() {
  Registry& r = reg();
  std::lock_guard<std::mutex> lock(r.mutex);
  return r.legacy.size();
}

// ── the environment audit ───────────────────────────────────────────────────────────────────────

EnvAudit audit_environment() {
  EnvAudit a;
  std::map<std::string, CVarBase*> vars;
  std::map<std::string, Registry::LegacyRead> legacy;
  {
    Registry& r = reg();
    std::lock_guard<std::mutex> lock(r.mutex);
    vars = r.vars;
    legacy = r.legacy;
  }
  for (const std::string& entry : lucent::config::active()) {
    if (!is_psxport_name(entry)) continue;
    a.set_in_env.push_back(entry);
    const std::string name = name_of_entry(entry);
    if (vars.count(name))
      a.declared.push_back(name);
    else if (legacy.count(name))
      a.legacy.push_back(name);
    else
      a.unknown.push_back(name);
  }
  return a;
}

void report() {
  const std::size_t n_declared = registered_count();
  lucent::info("cfg", "{} CVar(s) declared, {} knob(s) observed through the legacy cfg_* path",
               n_declared, legacy_read_count());

  enumerate([](CVarBase& v) {
    lucent::Line ln;
    ln.add("  {} = {} [{}]", v.name(), v.value_text(), layer_name(v.layer()));
    // Show the layers BELOW the winner too — "it is 1 because the env says so, and your settings
    // file says 0" is the answer people actually need, and it is one line.
    for (int i = 1; i < kLayerCount; ++i) {
      const Layer l = (Layer)i;
      if (l != v.layer() && v.has(l)) ln.add("  ({}={})", layer_name(l), v.layer_text(l));
    }
    if (v.external()) ln.add("  (external: resolved by lucent, not by this registry)");
    ln.flush(lucent::Level::Info, "cfg");
  });

  const EnvAudit a = audit_environment();
  // THE NEGATIVE CARRIES ITS DENOMINATOR AND ITS BLIND SPOT. "0 unknown" printed on its own is
  // indistinguishable from an audit that never looked at anything.
  lucent::info("cfg",
               "env audit: {} PSXPORT_* variable(s) set -> {} declared, {} legacy (observed when "
               "read), {} UNKNOWN",
               a.set_in_env.size(), a.declared.size(), a.legacy.size(), a.unknown.size());
  for (const std::string& u : a.unknown)
    lucent::warn("cfg", "UNKNOWN knob {} is set and matched nothing — it did NOTHING in this run", u);
  lucent::info("cfg",
               "env audit BLIND SPOT: only {} of the port's knobs are declared CVars. An un-migrated "
               "knob is recognised only once something READS it, so one on a code path this run never "
               "entered is counted UNKNOWN here. Re-run the audit at exit for the honest number.",
               n_declared);
}

void report_exit_audit() {
  // The boot-time audit's blind spot is real and was measured on the first run that used it:
  // PSXPORT_VK_HEADLESS came back UNKNOWN because gpu_vk does not initialise until after the boot
  // report, and PSXPORT_NOWINDOW came back UNKNOWN because it is genuinely read by nothing in the
  // binary (only by run.sh, which translates it). Only one of those two is a real finding, and at
  // exit the difference is decided rather than caveated: everything that was going to be read has
  // been.
  //
  // LIMITATION, stated because a diagnostic that can print nothing is lying: this runs from
  // std::atexit, so it does NOT fire when the process is killed — and an agent's `timeout`-bounded
  // headless run ends in SIGTERM, which watchdog.cpp's handler answers with _exit(130). For those
  // runs the boot audit plus its blind-spot line is all you get; ask the live process instead, with
  // `cvars` over the debug server.
  const EnvAudit a = audit_environment();
  lucent::info("cfg",
               "env audit AT EXIT (everything that was going to be read has been): {} PSXPORT_* set "
               "-> {} declared, {} legacy, {} UNKNOWN",
               a.set_in_env.size(), a.declared.size(), a.legacy.size(), a.unknown.size());
  for (const std::string& u : a.unknown)
    lucent::warn("cfg", "UNKNOWN knob {} was set for this whole run and NOTHING ever read it", u);
}

void report_once() {
  static bool done = false;
  if (done) return;
  done = true;
  report();
  std::atexit(report_exit_audit);
}

bool selftest() {
  // Run the classifier against BOTH classes. A name that must come back UNKNOWN, and a declared name
  // that must not. Checking only the first would pass an audit that called everything unknown;
  // checking only the second would pass an audit that returned an empty list forever.
  static const char kProbe[] = "PSXPORT_CFG_SELFTEST_PROBE_NOT_A_KNOB";
  static const char kDeclared[] = "PSXPORT_CFG_SELFTEST_DECLARED";

  const bool probe_was_set = detail::env_present(kProbe);
  const bool declared_was_set = detail::env_present(kDeclared);
  if (probe_was_set || declared_was_set) {
    lucent::error("cfg", "selftest cannot run: {}/{} already set in the environment", kProbe, kDeclared);
    return false;
  }

  setenv(kProbe, "1", 1);
  setenv(kDeclared, "1", 1);
  lucent::config::reset_cache();
  const EnvAudit a = audit_environment();
  unsetenv(kProbe);
  unsetenv(kDeclared);
  lucent::config::reset_cache();

  const bool positive = contains(a.unknown, kProbe);
  const bool negative = contains(a.declared, kDeclared) && !contains(a.unknown, kDeclared);
  if (!positive)
    lucent::error("cfg", "selftest FAILED: an undeclared knob set in the environment was NOT reported ({} of {} scanned)",
                  a.unknown.size(), a.set_in_env.size());
  if (!negative)
    lucent::error("cfg", "selftest FAILED: a DECLARED knob set in the environment was misclassified ({} declared of {} scanned)",
                  a.declared.size(), a.set_in_env.size());
  return positive && negative;
}

// ── mutation from outside ───────────────────────────────────────────────────────────────────────

namespace {
bool set_layer_from_text(std::string_view name, std::string_view text, Layer l, const char* who) {
  CVarBase* v = find(name);
  if (!v) {
    lucent::warn("cfg", "{}: {} is not a declared CVar — ignored", who, std::string(name));
    return false;
  }
  if (v->external()) {
    lucent::warn("cfg", "{}: {} is resolved by lucent, not by this registry — ignored", who,
                 std::string(name));
    return false;
  }
  if (l == Layer::Value && !v->persistable()) {
    lucent::warn("cfg", "{}: {} is not persistable and has no Value layer — ignored", who,
                 std::string(name));
    return false;
  }
  if (!v->set_text(l, text)) {
    lucent::warn("cfg", "{}: \"{}\" is not a valid {} for {} — ignored", who, std::string(text),
                 kind_name(v->kind()), std::string(name));
    return false;
  }
  return true;
}
}  // namespace

bool set_from_settings_file(std::string_view name, std::string_view text) {
  return set_layer_from_text(name, text, Layer::Value, "settings");
}

bool set_runtime(std::string_view name, std::string_view text) {
  if (!set_layer_from_text(name, text, Layer::Runtime, "runtime")) return false;
  CVarBase* v = find(name);
  lucent::info("cfg", "runtime: {} = {} [{}] (this run only, never persisted)", std::string(name),
               v->value_text(), layer_name(v->layer()));
  return true;
}

void note_runtime_external(std::string_view name, std::string_view text) {
  // The REPL / debug-server `debug ...` path. The channel set is applied by lucent::enable_channels
  // and is NOT resolved from here; this records it at the Runtime layer so the dump can say what the
  // live channel set is and that a console command, not the environment, is where it came from.
  // Without this the one documented precedence rule in the whole repo — "a REPL `debug` still
  // overrides the environment for the rest of the run" — would remain a sentence in a doc with no
  // representation in the program.
  CVarBase* v = find(name);
  if (!v) {
    lucent::warn("cfg", "runtime note for {} dropped: no such CVar", std::string(name));
    return;
  }
  v->set_text(Layer::Runtime, text);
}

bool clear_runtime(std::string_view name) {
  CVarBase* v = find(name);
  if (!v) {
    lucent::warn("cfg", "runtime: {} is not a declared CVar — ignored", std::string(name));
    return false;
  }
  v->clear(Layer::Runtime);
  lucent::info("cfg", "runtime: {} cleared -> {} [{}]", std::string(name), v->value_text(),
               layer_name(v->layer()));
  return true;
}

void reset_for_test() {
  std::vector<CVarBase*> snapshot;
  {
    Registry& r = reg();
    std::lock_guard<std::mutex> lock(r.mutex);
    for (const auto& kv : r.vars) snapshot.push_back(kv.second);
  }
  for (CVarBase* v : snapshot) {
    v->clear(Layer::Override);
    v->mEnvBound = false;
  }
}

}  // namespace psx::config

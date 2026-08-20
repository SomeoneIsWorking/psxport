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
  std::map<std::string, CVarBase *> vars;
  // Knobs that no CVar declares but which some cfg_* call has READ this run, with what they
  // resolved to. This is the compatibility path's visibility: an un-migrated knob is invisible until
  // something reads it, which is honest and is why the audit reports it as its own class.
  struct LegacyRead {
    Kind kind;
    std::string resolved;
  };
  std::map<std::string, LegacyRead> legacy;
};

Registry &reg() {
  static Registry r;
  return r;
}

// The one place that decides what counts as one of our knobs. lucent::config::active() lists the
// whole environment when the prefix is empty (psxport never calls set_prefix — every call site
// passes the full PSXPORT_-prefixed name), so the filter lives here.
bool is_psxport_name(std::string_view s) {
  return s.rfind("PSXPORT_", 0) == 0;
}

std::string name_of_entry(const std::string &entry) {
  const std::size_t eq = entry.find('=');
  return eq == std::string::npos ? entry : entry.substr(0, eq);
}

bool contains(const std::vector<std::string> &v, std::string_view s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

// ── names ───────────────────────────────────────────────────────────────────────────────────────

const char *layer_name(Layer l) {
  switch (l) {
  case Layer::Default:
    return "default";
  case Layer::Value:
    return "value"; // psxport_settings.ini
  case Layer::Override:
    return "env"; // PSXPORT_* launch environment
  case Layer::Runtime:
    return "runtime"; // REPL / debug-server, this run only
  }
  return "?";
}

const char *kind_name(Kind k) {
  switch (k) {
  case Kind::Bool:
    return "bool";
  case Kind::Int:
    return "int";
  case Kind::Text:
    return "text";
  }
  return "?";
}

// ── detail: the forwarders and the registry hooks ───────────────────────────────────────────────

namespace detail {

// These four are the ENTIRE environment-reading surface of the CVar system, and each is exactly the
// expression the pre-migration cfg_* function used. That is what makes the compatibility claim
// structural rather than aspirational.
bool env_present(const char *name) {
  return lucent::config::present(name);
}
bool env_bool(const char *name) {
  return lucent::config::flag(name);
}
long env_int(const char *name, long fallback) {
  return lucent::config::number(name, fallback);
}
std::string env_text(const char *name) {
  return lucent::config::text(name);
}

void registry_lock() {
  reg().mutex.lock();
}
void registry_unlock() {
  reg().mutex.unlock();
}

void registry_add(CVarBase *v) {
  Registry &r = reg();
  std::lock_guard<std::mutex> lock(r.mutex);
  const auto it = r.vars.find(v->name());
  if (it != r.vars.end()) {
    // Two CVars claiming one name is a build-time mistake that would make `find()` return whichever
    // object won a race. Say so; do not silently keep one.
    lucent::error("cfg", "CVar name collision: {} is declared twice — the second declaration is ignored", v->name());
    return;
  }
  r.vars.emplace(v->name(), v);
}

void registry_remove(CVarBase *v) {
  Registry &r = reg();
  std::lock_guard<std::mutex> lock(r.mutex);
  const auto it = r.vars.find(v->name());
  if (it != r.vars.end() && it->second == v) {
    r.vars.erase(it);
  }
}

} // namespace detail

// ── CVarBase ────────────────────────────────────────────────────────────────────────────────────

CVarBase::CVarBase(const char *name, Kind kind, const char *help, bool persistable, bool external)
    : mName(name), mHelp(help), mKind(kind), mPersistable(persistable), mExternal(external) {
  detail::registry_add(this);
}

CVarBase::~CVarBase() {
  detail::registry_remove(this);
}

Layer CVarBase::layer() const {
  ensure_env_bound();
  for (int i = kLayerCount - 1; i >= 1; --i) {
    if (mSet[i]) {
      return (Layer)i;
    }
  }
  return Layer::Default;
}

bool CVarBase::has(Layer l) const {
  ensure_env_bound();
  return mSet[(int)l];
}

void CVarBase::ensure_env_bound() const {
  if (mEnvBound) {
    return;
  }
  // Set the flag BEFORE binding: bind_env() reads the environment, which can log, and a logging call
  // that came back here would recurse. Binding twice is harmless; recursing is not.
  mEnvBound = true;
  const_cast<CVarBase *>(this)->bind_env();
}

// ── CVar<T> ─────────────────────────────────────────────────────────────────────────────────────

template <> std::string CVar<bool>::to_text(const bool &v) {
  return v ? "true" : "false";
}
template <> std::string CVar<long>::to_text(const long &v) {
  return std::to_string(v);
}
template <> std::string CVar<std::string>::to_text(const std::string &v) {
  return v;
}

template <> bool CVar<bool>::set_text(Layer l, std::string_view text) {
  std::string s(text);
  for (char &c : s) {
    c = (char)tolower((unsigned char)c);
  }
  // Deliberately the same vocabulary lucent::config::flag accepts, so a value that means "off" in
  // the environment means "off" in the settings file and in a REPL command too.
  const bool off = s == "0" || s == "false" || s == "no" || s == "off";
  const bool on = s == "1" || s == "true" || s == "yes" || s == "on";
  if (!off && !on) {
    return false;
  }
  set(l, on);
  return true;
}

template <> bool CVar<long>::set_text(Layer l, std::string_view text) {
  const std::string s(text);
  char *end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 0);
  if (s.empty() || !end || *end != '\0') {
    return false;
  }
  set(l, v);
  return true;
}

template <> bool CVar<std::string>::set_text(Layer l, std::string_view text) {
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
template <> void CVar<bool>::bind_env() {
  if (!detail::env_present(mName)) {
    return;
  }
  mSlot[(int)Layer::Override - 1] = detail::env_bool(mName);
  mSet[(int)Layer::Override] = true;
}

template <> void CVar<long>::bind_env() {
  if (!detail::env_present(mName)) {
    return;
  }
  // The VALUE is whatever the pre-migration call would have produced — lucent::config::number with
  // this CVar's default as the fallback — so migrating cannot change what the run does. The warning
  // is the part that is new: a typo'd integer used to silently become the default, and a knob that
  // silently did not apply is the exact failure this system exists to make impossible.
  const std::string raw = detail::env_text(mName);
  char *end = nullptr;
  (void)std::strtol(raw.c_str(), &end, 0);
  if (raw.empty() || !end || *end != '\0') {
    lucent::warn("cfg", "{}=\"{}\" is not an integer — falling back to the default {}", mName, raw, mDefault);
  }
  mSlot[(int)Layer::Override - 1] = detail::env_int(mName, mDefault);
  mSet[(int)Layer::Override] = true;
}

template <> void CVar<std::string>::bind_env() {
  if (!detail::env_present(mName)) {
    return;
  }
  mSlot[(int)Layer::Override - 1] = detail::env_text(mName);
  mSet[(int)Layer::Override] = true;
}

template class CVar<bool>;
template class CVar<long>;
template class CVar<std::string>;

// ── THE INVENTORY ───────────────────────────────────────────────────────────────────────────────
// Declared in config_vars.h, which carries the per-knob documentation. Keep the two in the same
// order.
BoolVar cv_oracle("PSXPORT_ORACLE",
                  false,
                  "pure PSX reference mode: no native render enhancement",
                  /*persistable=*/false);
BoolVar cv_noaudio("PSXPORT_NOAUDIO", false, "open no audio device", /*persistable=*/false);
BoolVar cv_nopace("PSXPORT_NOPACE",
                  false,
                  "do not sleep to hold the present rate",
                  /*persistable=*/false);
BoolVar cv_repl("PSXPORT_REPL",
                false,
                "interactive REPL on stdin — SERVICED ONLY by the single-core native frame loop; "
                "REFUSED (exit 2) under the SBS harness (repl_service.h)",
                /*persistable=*/false);

IntVar cv_watchdog("PSXPORT_WATCHDOG",
                   3,
                   "frame-progress timeout, seconds (0 = off)",
                   /*persistable=*/false);
IntVar cv_watchdog_boot("PSXPORT_WATCHDOG_BOOT",
                        -1,
                        "first-present grace, seconds (-1 = derive: max(WATCHDOG, 45))",
                        /*persistable=*/false);

TextVar cv_asset_dir("PSXPORT_ASSET_DIR",
                     "",
                     "directory containing assets/ (empty = cwd-relative)",
                     /*persistable=*/false);
TextVar cv_settings_path("PSXPORT_SETTINGS",
                         "psxport_settings.ini",
                         "path to the settings file — selects the Value layer, so never persisted",
                         /*persistable=*/false);

// The producer census runs on every guest store and every prim. Its cost was recorded in ot_attr.h as
// "+0.26%" from a 300-frame run at 77.7 s — i.e. 3.9 fps, a pace at which almost anything looks free.
// A host profile of an unpaced 3D field scene puts OtAttr::resolveClaimedFrame at 13.31% and
// trackStoreSlow at 3.91%. Those two numbers cannot both describe the same thing, and until this knob
// existed there was no way to A/B it at all — the arm was a hardcoded `inline bool = true`.
// Default TRUE, so behaviour is unchanged and the census the USER asked for keeps working.
BoolVar cv_producers("PSXPORT_PRODUCERS",
                     true,
                     "arm the producer census (per-store + per-prim attribution). Off makes the "
                     "producer DB stop recording; use it to price the census, not to fix a bug",
                     /*persistable=*/false);

TextVar cv_producers_dir("PSXPORT_PRODUCERS_DIR",
                         "scratch/producers",
                         "where the producer-census JSONL + accumulated claim set are written",
                         /*persistable=*/false);
TextVar cv_producers_db("PSXPORT_PRODUCERS_DB",
                        "",
                        "claim set the guest leg resolves against (flat claims file or a run JSONL); "
                        "empty = <PRODUCERS_DIR>/claims.txt",
                        /*persistable=*/false);

BoolVar cv_fps60("PSXPORT_FPS60", false, "interpolated-60fps tier (Value layer = fps60= in the settings file)");

TextVar cv_enh("PSXPORT_ENH",
               "",
               "pc_enh selection: <name,name|all> — deliberate guest-state changes. "
               "FORCE-SUPPRESSED under PSXPORT_ORACLE or either SBS form; read it through "
               "enh_named()/enh(), never by parsing this text at a call site.");

// ── the enhancement gate ────────────────────────────────────────────────────────────────────────
// THE DEFINITION OF A BYTE-COMPARE RUN, in one place. Pure in its three inputs so selftest() can run
// it over all eight combinations without touching the environment.
//
// PSXPORT_SBS and PSXPORT_SBS_MODE are deliberately NOT migrated here. They belong to the SBS harness
// group (docs/config-migration.md), and migrating a knob as a side effect of migrating a different one
// is how a group ends up half-done with no gate on it. compare_run() reads them through
// detail::env_bool / detail::env_text — the same 1:1 forwarders cfg_on / cfg_str resolve through — and
// notes the read, so they appear in the audit as legacy-observed rather than as unknown.
//
// THAT IS CHEAP ONLY UNTIL THEY ARE MIGRATED, which docs/config-migration.md Group 4a lists as
// remaining work. On the commit that declares either as a CVar, THIS FUNCTION MUST CHANGE WITH IT:
// reading detail::env_* would bypass the ladder for exactly the two inputs that decide whether a
// byte-compare is protected (a settings-file or REPL value for them would never reach the gate), and
// the one-time binding below would freeze a value that can now move mid-run — the pre-migration
// cfg_enh seeded-static defect, one layer down. tests/test_config_enh.cpp's
// test_migrating_the_sbs_knobs_must_update_compare_run goes RED when either name becomes a CVar, so
// this note cannot be missed rather than merely being written down.
bool compare_run_from(bool oracle, bool sbs, std::string_view sbs_mode, std::string *why) {
  if (why) {
    why->clear();
  }
  const bool mode = !sbs_mode.empty();
  if (why) {
    if (oracle) {
      why->append(why->empty() ? "" : ", ").append("PSXPORT_ORACLE");
    }
    if (sbs) {
      why->append(why->empty() ? "" : ", ").append("PSXPORT_SBS");
    }
    if (mode) {
      why->append(why->empty() ? "" : ", ").append("PSXPORT_SBS_MODE=").append(sbs_mode);
    }
  }
  return oracle || sbs || mode;
}

namespace {

// The gate's whole mutable state, behind ONE mutex. Three parts, and each is here rather than in a
// local static for a stated reason:
//
//  * THE SBS ENVIRONMENT CACHE. compare_run() is called on every gate call, and note_legacy_read()
//    takes the registry lock and inserts into a map — paying that per call on a per-frame gate is not
//    acceptable. The two SBS names are ENV-ONLY knobs (they are not CVars, so no Value or Runtime layer
//    can move them mid-run), so binding them once is not a shortcut, it is their actual lifetime; the
//    legacy-read note is recorded once, which is exactly what "this knob was read this run" means.
//    cv_oracle is NOT cached here — it is a CVar and the REPL can move it, so it is read every call.
//  * THE WARN-ONCE REGISTERS, keyed on the KNOB. Two of them, because a run can legitimately have one
//    enhancement suppressed and another honoured, and neither announcement may silence the other.
//    Keyed by NAME, since the name is a knob's identity (a cfg_enh caller passes only a name, and the
//    same knob must not warn twice through the two entry points) — and a vector has no fixed capacity
//    to overflow silently.
//  * THE PARSED PSXPORT_ENH LIST, cached against the TEXT the ladder resolved rather than behind a
//    one-shot "seeded" flag. A Runtime-layer (REPL) write changes the text and the cache must notice.
//    That one-shot seeding is precisely the pre-migration defect: after the first call nothing could
//    change the answer for the rest of the process.
struct EnhState {
  std::mutex mutex;
  bool sbs_bound = false;
  bool sbs = false;
  std::string sbs_mode;
  std::vector<std::string> warned_suppressed;
  std::vector<std::string> warned_active;
  bool warned_nameless = false;
  std::string parsed_text = "\x01"; // a value no environment can produce: the first call always parses
  bool parsed_all = false;
  std::vector<std::string> parsed_names;
};
EnhState &enh_state() {
  static EnhState s;
  return s;
}

// True the FIRST time this key is seen in `r`; false afterwards. Caller holds the lock.
bool first_time(std::vector<std::string> &r, const char *key) {
  if (contains(r, key)) {
    return false;
  }
  r.emplace_back(key);
  return true;
}

} // namespace

bool compare_run(std::string *why) {
  EnhState &s = enh_state();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.sbs_bound) {
    s.sbs_bound = true;
    s.sbs = detail::env_bool("PSXPORT_SBS");
    s.sbs_mode = detail::env_text("PSXPORT_SBS_MODE");
    note_legacy_read("PSXPORT_SBS", Kind::Bool, s.sbs ? "1" : "0");
    note_legacy_read("PSXPORT_SBS_MODE", Kind::Text, s.sbs_mode);
  }
  return compare_run_from(cv_oracle.get(), s.sbs, s.sbs_mode, why);
}

bool enh_gate(const char *key, bool asked) {
  // A NAMELESS enhancement is refused, LOUDLY — the one place this deliberately departs from the
  // pre-migration behaviour, which returned 1 for cfg_enh("") whenever PSXPORT_ENH=all because `all`
  // short-circuited before it ever looked at the name. That made an empty or unexpanded name at a call
  // site read as a working enabled enhancement, and it cannot be carried forward: every notice here is
  // keyed on the knob's identity, so a nameless key would produce an unattributable log line and a
  // register entry nothing can match. tests/test_config_enh.cpp asserts this divergence explicitly
  // rather than letting the compatibility gate paper over it.
  if (!key || !*key) {
    EnhState &s = enh_state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.warned_nameless) {
      s.warned_nameless = true;
      lucent::error("cfg",
                    "enhancement gate called with an EMPTY name — refused. An enhancement is "
                    "identified by its name; a call site with none is a bug at the call site, "
                    "not a knob that is off.");
    }
    return false;
  }
  std::string why;
  const bool compare = compare_run(&why);
  EnhState &s = enh_state();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (compare) {
    // NOT announced when the knob was never asked for: an enhancement nobody requested being off is
    // not news. When it WAS asked for, the run must say so PER KNOB — otherwise a run with two
    // enhancements set names one of them and the other reads as never having been requested.
    if (asked && first_time(s.warned_suppressed, key)) {
      lucent::warn("cfg", "{} SUPPRESSED: oracle/SBS run must stay enhancement-free ({})", key, why);
    }
    return false;
  }
  // THE POSITIVE PRINTS TOO. "No SUPPRESSED line" is otherwise indistinguishable from "the gate was
  // never reached", and a pc_enh changes canon guest state — a run that had one on must name it, or a
  // later byte-compare against that run reads the deliberate divergence as a port bug.
  if (asked && first_time(s.warned_active, key)) {
    lucent::info("cfg",
                 "{} ENHANCEMENT ACTIVE: this run deliberately diverges from recomp_path "
                 "(pc_enh, affect=full) — see docs/behavior-map.md",
                 key);
  }
  return asked;
}

bool enh(const CVar<bool> &v) {
  return enh_gate(v.name(), v.get());
}

bool enh_named(const char *name) {
  if (!name || !*name) {
    return enh_gate(name, false); // refused, and it says so — see enh_gate
  }
  const std::string text = cv_enh.get();
  bool selected = false;
  {
    EnhState &s = enh_state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.parsed_text != text) {
      s.parsed_text = text;
      s.parsed_all = (text == "all");
      s.parsed_names.clear();
      if (!s.parsed_all && !text.empty()) {
        // The pre-migration splitter, verbatim: the separators are `,`, `:` and space. It is compared
        // against the old body over 36 (value, name) pairs in tests/test_config_enh.cpp — that
        // comparison, not this comment, is what keeps the parse identical.
        std::size_t start = 0;
        while (start <= text.size()) {
          const std::size_t sep = text.find_first_of(",: ", start);
          const std::size_t end = (sep == std::string::npos) ? text.size() : sep;
          if (end > start) {
            s.parsed_names.emplace_back(text.substr(start, end - start));
          }
          if (sep == std::string::npos) {
            break;
          }
          start = sep + 1;
        }
      }
    }
    selected = s.parsed_all || contains(s.parsed_names, name);
  }
  return enh_gate(name, selected);
}

// PSXPORT_RENDER_PATH — the RENDER PATH tri-state: native | gte | psx
// (docs/plans/render-path-tristate.md). A TextVar rather than an int so the settings file and the REPL
// both read as the thing they select, and so a typo is REJECTED by render_path_parse instead of
// resolving to a plausible number. Persistable: the F1 overlay writes the Value layer, like fps60.
TextVar cv_render_path("PSXPORT_RENDER_PATH",
                       "native",
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
    lucent::warn("cfg",
                 "PSXPORT_RENDER_PATH='{}' matched NO render path — falling back to 'native'. "
                 "Valid: native | gte | psx.",
                 s);
    return RenderPath::Native;
  }
  return p;
}

TextVar cv_debug_channels("PSXPORT_DEBUG",
                          "",
                          "enabled diagnostic channels — READ BY LUCENT, declared here for visibility",
                          /*persistable=*/false,
                          /*external=*/true);
TextVar cv_log_file("PSXPORT_LOG_FILE",
                    "",
                    "log output path — READ BY LUCENT, declared here for visibility",
                    /*persistable=*/false,
                    /*external=*/true);

BoolVar cv_selftest_declared("PSXPORT_CFG_SELFTEST_DECLARED",
                             false,
                             "reserved: the environment audit's calibration target, configures nothing",
                             /*persistable=*/false);

// ── lookup ──────────────────────────────────────────────────────────────────────────────────────

CVarBase *find(std::string_view name) {
  Registry &r = reg();
  std::lock_guard<std::mutex> lock(r.mutex);
  const auto it = r.vars.find(std::string(name));
  return it == r.vars.end() ? nullptr : it->second;
}

void enumerate(const std::function<void(CVarBase &)> &fn) {
  // Copy under the lock, then call: a callback that reads a CVar takes the lock itself.
  std::vector<CVarBase *> snapshot;
  {
    Registry &r = reg();
    std::lock_guard<std::mutex> lock(r.mutex);
    for (const auto &kv : r.vars) {
      snapshot.push_back(kv.second);
    }
  }
  for (CVarBase *v : snapshot) {
    fn(*v);
  }
}

std::size_t registered_count() {
  Registry &r = reg();
  std::lock_guard<std::mutex> lock(r.mutex);
  return r.vars.size();
}

// ── the legacy compatibility path ───────────────────────────────────────────────────────────────

void note_legacy_read(const char *name, Kind kind, std::string_view resolved) {
  if (!name || !is_psxport_name(name)) {
    return;
  }
  Registry &r = reg();
  std::lock_guard<std::mutex> lock(r.mutex);
  r.legacy[name] = Registry::LegacyRead{kind, std::string(resolved)};
}

std::size_t legacy_read_count() {
  Registry &r = reg();
  std::lock_guard<std::mutex> lock(r.mutex);
  return r.legacy.size();
}

// ── the environment audit ───────────────────────────────────────────────────────────────────────

EnvAudit audit_environment() {
  EnvAudit a;
  std::map<std::string, CVarBase *> vars;
  std::map<std::string, Registry::LegacyRead> legacy;
  {
    Registry &r = reg();
    std::lock_guard<std::mutex> lock(r.mutex);
    vars = r.vars;
    legacy = r.legacy;
  }
  for (const std::string &entry : lucent::config::active()) {
    if (!is_psxport_name(entry)) {
      continue;
    }
    a.set_in_env.push_back(entry);
    const std::string name = name_of_entry(entry);
    if (vars.count(name)) {
      a.declared.push_back(name);
    } else if (legacy.count(name)) {
      a.legacy.push_back(name);
    } else {
      a.unknown.push_back(name);
    }
  }
  return a;
}

void report() {
  const std::size_t n_declared = registered_count();
  lucent::info(
      "cfg", "{} CVar(s) declared, {} knob(s) observed through the legacy cfg_* path", n_declared, legacy_read_count());

  enumerate([](CVarBase &v) {
    lucent::Line ln;
    ln.add("  {} = {} [{}]", v.name(), v.value_text(), layer_name(v.layer()));
    // Show the layers BELOW the winner too — "it is 1 because the env says so, and your settings
    // file says 0" is the answer people actually need, and it is one line.
    for (int i = 1; i < kLayerCount; ++i) {
      const Layer l = (Layer)i;
      if (l != v.layer() && v.has(l)) {
        ln.add("  ({}={})", layer_name(l), v.layer_text(l));
      }
    }
    if (v.external()) {
      ln.add("  (external: resolved by lucent, not by this registry)");
    }
    ln.flush(lucent::Level::Info, "cfg");
  });

  const EnvAudit a = audit_environment();
  // THE NEGATIVE CARRIES ITS DENOMINATOR AND ITS BLIND SPOT. "0 unknown" printed on its own is
  // indistinguishable from an audit that never looked at anything.
  lucent::info("cfg",
               "env audit: {} PSXPORT_* variable(s) set -> {} declared, {} legacy (observed when "
               "read), {} UNKNOWN",
               a.set_in_env.size(),
               a.declared.size(),
               a.legacy.size(),
               a.unknown.size());
  for (const std::string &u : a.unknown) {
    lucent::warn("cfg", "UNKNOWN knob {} is set and matched nothing — it did NOTHING in this run", u);
  }
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
               a.set_in_env.size(),
               a.declared.size(),
               a.legacy.size(),
               a.unknown.size());
  for (const std::string &u : a.unknown) {
    lucent::warn("cfg", "UNKNOWN knob {} was set for this whole run and NOTHING ever read it", u);
  }
}

void report_once() {
  static bool done = false;
  if (done) {
    return;
  }
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
  if (!positive) {
    lucent::error("cfg",
                  "selftest FAILED: an undeclared knob set in the environment was NOT reported ({} of {} scanned)",
                  a.unknown.size(),
                  a.set_in_env.size());
  }
  if (!negative) {
    lucent::error(
        "cfg",
        "selftest FAILED: a DECLARED knob set in the environment was misclassified ({} declared of {} scanned)",
        a.declared.size(),
        a.set_in_env.size());
  }

  // ── PART 1: the DEFINITION of a byte-compare run, over ALL EIGHT input combinations ──────────
  // compare_run_from() is the pure form of that definition, so this loop can reach the four
  // combinations the live process cannot (PSXPORT_SBS / PSXPORT_SBS_MODE are env-only knobs; nothing
  // in-process can move them). Both classes are required: the positive (every combination with any
  // input set must suppress, and must NAME the input that did it) and the negative (the one
  // combination with nothing set must NOT suppress). A gate only ever checked on the class it is
  // supposed to block would pass if it blocked everything — which is a port that silently cannot
  // enable an enhancement at all.
  //
  // THIS LOOP IS NOT SUFFICIENT ON ITS OWN, and that was measured rather than reasoned: the verifier
  // deleted the suppression from enh_gate() outright (`const bool compare = false && compare_run(&why)`)
  // and this loop still reported "0 disagreement(s)", because it never called the function the game
  // reaches. PART 2 below is what closes that hole; this part is kept for the coverage it adds.
  int enh_cases = 0, enh_bad = 0;
  for (int bits = 0; bits < 8; ++bits) {
    const bool oracle = (bits & 1) != 0, sbs = (bits & 2) != 0, mode = (bits & 4) != 0;
    std::string why;
    const bool got = compare_run_from(oracle, sbs, mode ? std::string_view("panes") : std::string_view(), &why);
    const bool want = oracle || sbs || mode;
    const bool named = want ? (why.find("PSXPORT_") != std::string::npos) : why.empty();
    ++enh_cases;
    if (got != want || !named) {
      ++enh_bad;
      lucent::error("cfg",
                    "selftest FAILED: compare_run(oracle={} sbs={} sbs_mode={}) = {} (want {}), "
                    "why=\"{}\"",
                    oracle,
                    sbs,
                    mode ? "panes" : "",
                    got,
                    want,
                    why);
    }
  }
  // The DENOMINATOR, printed whether or not anything failed: "the definition is fine" and "the
  // definition was never exercised" must not read the same.
  lucent::info("cfg",
               "selftest: compare_run_from() exercised over {} of 8 input combination(s) "
               "(1 must honour, 7 must suppress) -> {} disagreement(s)",
               enh_cases,
               enh_bad);
  const bool defn_ok = (enh_cases == 8) && (enh_bad == 0);

  // ── PART 2: THE SHIPPING PATH — enh_gate() itself, driven mid-process ─────────────────────────
  // The function the game actually reaches, exercised over both classes, in ONE process, with the
  // suppression input MOVED BETWEEN CASES AND NOTHING RESET IN BETWEEN. That last clause is the whole
  // design, and it gates two properties that were previously asserted only in comments:
  //
  //   * enh_gate() consults compare_run() at all. Delete the suppression and case 2 honours an
  //     enhancement under PSXPORT_ORACLE — which is a CONTAMINATED byte-compare oracle.
  //   * cv_oracle is re-read on EVERY call, so the REPL can still turn the oracle on (or off) after
  //     the first gate call. Bind it once and case 2 keeps case 1's answer.
  //
  // The lever is the RUNTIME LAYER of PSXPORT_ORACLE — i.e. set_runtime(), the REPL's own entry point,
  // not a test-only back door — because a CVar's environment Override is bound once per process and
  // cannot be moved by setenv() after the fact. Whatever the Runtime layer held is restored at the end.
  //
  // WHY THIS REFUSES IN A COMPARE RUN instead of adapting to one: reaching the HONOUR class would mean
  // forcing PSXPORT_ORACLE off in a process that IS an oracle run, transiently un-suppressing every
  // enhancement in the run this gate exists to protect. The other direction (a plain run transiently
  // looking like an oracle run for the duration of case 2) suppresses rather than contaminates, which
  // is why it is allowed. So in a compare run the selftest FAILS and says what it could not exercise —
  // it does not quietly return true over an unexercised gate.
  // The key is NOT a knob and its name says so, because driving the real gate makes the real gate LOG:
  // the lines below are a genuine `ENHANCEMENT ACTIVE` and a genuine `SUPPRESSED` notice, and a reader
  // scanning a boot log must not read them as this run's configuration. Hence the bracketing line too —
  // an announcement whose absence would make the selftest's own output indistinguishable from the run's.
  static const char kEnhKey[] = "PSXPORT_CFG_SELFTEST_ENH_NOT_A_REAL_KNOB";
  bool gate_ok = false;
  int gate_cases = 0, gate_bad = 0, gate_honour = 0, gate_suppress = 0;
  {
    std::string live_why;
    if (compare_run(&live_why)) {
      lucent::error("cfg",
                    "selftest FAILED: the enhancement gate cannot be exercised in a byte-compare run "
                    "({}). Reaching the HONOUR class would mean forcing the oracle off in a run that "
                    "IS one. NOTHING of enh_gate() was checked — run the selftest in a plain process.",
                    live_why);
    } else {
      lucent::info("cfg",
                   "selftest: driving enh_gate() with the synthetic knob {} — every runtime:/"
                   "ENHANCEMENT ACTIVE/SUPPRESSED line until the summary below is the SELFTEST's, not "
                   "this run's configuration. PSXPORT_ORACLE's Runtime layer is moved and restored.",
                   kEnhKey);
      // What the Runtime layer held before, so a REPL `oracle 1` typed earlier in this run survives.
      const bool had_runtime = cv_oracle.has(Layer::Runtime);
      const std::string prior_runtime = cv_oracle.layer_text(Layer::Runtime);

      // -1 = clear the Runtime layer, 0/1 = set it. `want` is what enh_gate(key, asked=true) must
      // return. Cases 2 and 3 move the input in BOTH directions, so a one-way latch fails one of them.
      struct GateCase {
        const char *what;
        int runtime;
        bool want;
      };
      static const GateCase kGate[] = {
          {"plain run, Runtime layer clear", -1, true},
          {"PSXPORT_ORACLE moved to 1 mid-process (no reset)", 1, false},
          {"PSXPORT_ORACLE moved back to 0 mid-process", 0, true},
      };
      int want_honour = 0, want_suppress = 0;
      for (const GateCase &g : kGate) {
        (g.want ? want_honour : want_suppress)++;
      }

      for (const GateCase &g : kGate) {
        const bool moved =
            (g.runtime < 0) ? clear_runtime("PSXPORT_ORACLE") : set_runtime("PSXPORT_ORACLE", g.runtime ? "1" : "0");
        // Both classes of `asked` through the hook itself, plus the PSXPORT_ENH-token entry point
        // enh_named(), which drags the parse and the cv_enh ladder read through with it. enh(CVar) is
        // NOT reached from here — it is the one-line forwarder enh_gate(v.name(), v.get()) and would
        // need a knob declared in the registry purely to call it; tests/test_config_enh.cpp case 7
        // drives it over both classes instead. Stated because an unlisted entry point reads as covered.
        const bool asked = enh_gate(kEnhKey, true);
        const bool unasked = enh_gate(kEnhKey, false);
        const bool named = enh_named(kEnhKey); // resolves through cv_enh; unset -> not selected
        const bool ok = moved && asked == g.want && !unasked && !named;
        ++gate_cases;
        (g.want ? gate_honour : gate_suppress)++;
        if (!ok) {
          ++gate_bad;
          lucent::error("cfg",
                        "selftest FAILED: {} -> enh_gate(asked=true) = {} (want {}), "
                        "enh_gate(asked=false) = {} (want 0), enh_named() = {} (want 0), "
                        "ladder move accepted = {}",
                        g.what,
                        asked,
                        g.want,
                        unasked,
                        named,
                        moved);
        }
      }

      if (had_runtime) {
        cv_oracle.set_text(Layer::Runtime, prior_runtime);
      } else {
        cv_oracle.clear(Layer::Runtime);
      }

      // The denominator AND the blind spot, printed on the way through whether or not anything failed.
      lucent::info("cfg",
                   "selftest: enh_gate() — THE SHIPPING PATH — driven over {} configuration(s) in one "
                   "process ({} must honour, {} must suppress; expected {}/{}) -> {} disagreement(s). "
                   "BLIND SPOT: only cv_oracle can be moved in-process, so the SBS half of the "
                   "suppression is covered by compare_run_from() above and by "
                   "tests/test_config_enh.cpp; the notice TEXT needs a log sink and is gated there too.",
                   gate_cases,
                   gate_honour,
                   gate_suppress,
                   want_honour,
                   want_suppress,
                   gate_bad);
      gate_ok =
          (gate_cases == 3) && (gate_bad == 0) && (gate_honour == want_honour) && (gate_suppress == want_suppress);
    }
  }

  return positive && negative && defn_ok && gate_ok;
}

// ── mutation from outside ───────────────────────────────────────────────────────────────────────

namespace {
bool set_layer_from_text(std::string_view name, std::string_view text, Layer l, const char *who) {
  CVarBase *v = find(name);
  if (!v) {
    lucent::warn("cfg", "{}: {} is not a declared CVar — ignored", who, std::string(name));
    return false;
  }
  if (v->external()) {
    lucent::warn("cfg", "{}: {} is resolved by lucent, not by this registry — ignored", who, std::string(name));
    return false;
  }
  if (l == Layer::Value && !v->persistable()) {
    lucent::warn("cfg", "{}: {} is not persistable and has no Value layer — ignored", who, std::string(name));
    return false;
  }
  if (!v->set_text(l, text)) {
    lucent::warn("cfg",
                 "{}: \"{}\" is not a valid {} for {} — ignored",
                 who,
                 std::string(text),
                 kind_name(v->kind()),
                 std::string(name));
    return false;
  }
  return true;
}
} // namespace

bool set_from_settings_file(std::string_view name, std::string_view text) {
  return set_layer_from_text(name, text, Layer::Value, "settings");
}

bool set_runtime(std::string_view name, std::string_view text) {
  if (!set_layer_from_text(name, text, Layer::Runtime, "runtime")) {
    return false;
  }
  CVarBase *v = find(name);
  lucent::info("cfg",
               "runtime: {} = {} [{}] (this run only, never persisted)",
               std::string(name),
               v->value_text(),
               layer_name(v->layer()));
  return true;
}

void note_runtime_external(std::string_view name, std::string_view text) {
  // The REPL / debug-server `debug ...` path. The channel set is applied by lucent::enable_channels
  // and is NOT resolved from here; this records it at the Runtime layer so the dump can say what the
  // live channel set is and that a console command, not the environment, is where it came from.
  // Without this the one documented precedence rule in the whole repo — "a REPL `debug` still
  // overrides the environment for the rest of the run" — would remain a sentence in a doc with no
  // representation in the program.
  CVarBase *v = find(name);
  if (!v) {
    lucent::warn("cfg", "runtime note for {} dropped: no such CVar", std::string(name));
    return;
  }
  v->set_text(Layer::Runtime, text);
}

bool clear_runtime(std::string_view name) {
  CVarBase *v = find(name);
  if (!v) {
    lucent::warn("cfg", "runtime: {} is not a declared CVar — ignored", std::string(name));
    return false;
  }
  v->clear(Layer::Runtime);
  lucent::info("cfg", "runtime: {} cleared -> {} [{}]", std::string(name), v->value_text(), layer_name(v->layer()));
  return true;
}

void reset_for_test() {
  std::vector<CVarBase *> snapshot;
  {
    Registry &r = reg();
    std::lock_guard<std::mutex> lock(r.mutex);
    for (const auto &kv : r.vars) {
      snapshot.push_back(kv.second);
    }
  }
  for (CVarBase *v : snapshot) {
    v->clear(Layer::Override);
    v->mEnvBound = false;
  }
  // The enhancement gate's state is an environment binding too — the SBS cache literally is one, and
  // the warn-once registers are "what this configuration has already announced". A test that
  // re-configures the environment and then measures the previous configuration's silence would be
  // measuring the reset it forgot to ask for, so this is not separable.
  EnhState &s = enh_state();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.sbs_bound = false;
  s.sbs = false;
  s.sbs_mode.clear();
  s.warned_suppressed.clear();
  s.warned_active.clear();
  s.warned_nameless = false;
  s.parsed_text = "\x01";
  s.parsed_all = false;
  s.parsed_names.clear();
}

} // namespace psx::config

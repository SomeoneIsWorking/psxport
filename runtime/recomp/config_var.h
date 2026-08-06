#ifndef PSXPORT_CONFIG_VAR_H
#define PSXPORT_CONFIG_VAR_H
// config_var.h — the CVar type and READ access. Nothing here mutates the registry.
//
// A CVar is one configuration knob. Its value comes from one of four LAYERS, and the whole point of
// the type is that the question "where did this value come from?" has an answer you can print:
//
//     Default   what the framework compiled in
//   < Value     the user's persisted choice (psxport_settings.ini)
//   < Override  a launch argument — the PSXPORT_* environment variable. Never persisted.
//   < Runtime   a REPL / debug-server command. This run only. Never persisted.
//
// Before this existed, psxport had 201 PSXPORT_* environment knobs, a separate settings file, and
// REPL `debug ...` commands, with exactly one sentence of documented precedence between the three
// (docs/config.md: a REPL `debug` "still overrides the environment for the rest of the run").
// Everything else was whichever `getenv` happened to run.
//
// TAKEN FROM DUSKLIGHT (src/dusk/config_var.hpp, CC0), and the deviations, which are deliberate:
//  * The layer ladder itself, the "CVars, like ogres, have layers" model, and the rule that the
//    upper layers are NEVER written back to the config file (their getValueForSave -> our
//    value_for_save).
//  * Their header split: this file is "just access", safe to include widely; config.h carries
//    mutation, loading and the registry. HONEST NOTE ON WHETHER IT EARNS ITS KEEP HERE: their
//    argument is compile time — their config.hpp drags in nlohmann/json. Ours drags in nothing
//    heavier than <functional>, so that argument does not transfer. It is kept for a different and
//    still-good reason: it keeps the MUTATION surface small and greppable, so "who can write the
//    Value layer of this knob" is a one-line grep with a short answer.
//  * DEVIATION — the ladder's top. Dusklight has Default < Value < Speedrun < Override, with the
//    launch argument outranking their temporary Speedrun mode. Ours puts Runtime ABOVE Override,
//    because psxport's runtime layer is a human typing at a live console after launch, which is
//    later and more specific than the environment the process started in — and because that is what
//    the REPL already did. Changing it would have silently altered a documented behaviour.
//  * DEVIATION — storage. Dusklight keeps three value slots plus a `priorLayer` field to remember
//    what to fall back to. We keep one std::optional per layer, so `layer()` is DERIVED from what is
//    actually set rather than stored alongside it. That is what makes the introspection dump able to
//    show every layer of a knob at once, which is the whole reason this exists.
//  * DEVIATION — registration. Dusklight requires an explicit Register() and aborts on access to an
//    unregistered CVar. Ours self-registers in the constructor against a function-local static
//    registry. psxport's CVars are file-scope globals in several translation units, so a two-phase
//    scheme would reintroduce exactly the failure this repo already paid for once: an initialisation
//    step that is never reached, leaving a diagnostic silently switched off (see
//    tests/test_lucent_channel_env.cpp). A constructor cannot fail to run.
//
// THREADING. CVars are configured during startup (defaults, settings file, environment) and
// afterwards only from the REPL / debug-server, which run on the game thread between frames. Reads
// are lock-free once a CVar has bound its environment Override; mutation takes the registry lock.
// A REPL write racing a read of a TextVar on another thread is the same exposure
// lucent::enable_channels already has, and is not made worse here.
#include <optional>
#include <string>
#include <string_view>

namespace psx::config {

// The ladder. Ordered: a higher enumerator wins. `Default` is index 0 and is stored separately (a
// CVar always has one), so the optional slots below are indexed by `(int)layer - 1`.
enum class Layer : unsigned char { Default = 0, Value = 1, Override = 2, Runtime = 3 };
inline constexpr int kLayerCount = 4;
const char* layer_name(Layer l);

// What a knob holds. Deliberately only the three shapes the cfg_* API ever had — bool, integer,
// string — so every one of the 201 existing knobs is expressible without a new type.
enum class Kind : unsigned char { Bool, Int, Text };
const char* kind_name(Kind k);

class CVarBase;

namespace detail {
// 1:1 forwarders onto lucent::config, which is what cfg_on/cfg_int/cfg_str WERE. Routing the
// environment read through these — rather than re-parsing the variable here — is what makes
// "a migrated knob resolves identically to its pre-migration behaviour" true by construction
// instead of true by careful re-implementation. Defined in config.cpp.
bool        env_present(const char* name);
bool        env_bool(const char* name);
long        env_int(const char* name, long fallback);
std::string env_text(const char* name);
// Registry hooks. Declared here so a CVar can self-register and self-unregister without this header
// pulling in the registry's own interface.
void registry_add(CVarBase* v);
void registry_remove(CVarBase* v);
void registry_lock();
void registry_unlock();
}  // namespace detail

class CVarBase {
 public:
  virtual ~CVarBase();

  const char* name() const { return mName; }
  Kind kind() const { return mKind; }
  const char* help() const { return mHelp; }

  // False for a knob whose Value layer must never be written to the settings file — e.g.
  // PSXPORT_SETTINGS, which SELECTS that file and so cannot be stored inside it.
  bool persistable() const { return mPersistable; }

  // True for a knob that is DECLARED here for introspection but whose value is resolved by another
  // subsystem — PSXPORT_DEBUG and PSXPORT_LOG_FILE, which lucent reads for itself (built with
  // LUCENT_CHANNEL_ENV / LUCENT_LOG_FILE_ENV; see cmake/psxport.cmake). Declaring them costs nothing
  // and keeps the environment audit from reporting the two most-used knobs in the port as unknown.
  // Nothing here reads or overrides them.
  bool external() const { return mExternal; }

  // The layer the effective value comes from — derived from which slots are set, not stored.
  Layer layer() const;
  bool has(Layer l) const;

  // Introspection. Both must work for every Kind, because the dump is the point.
  virtual std::string value_text() const = 0;        // the effective value, printable
  virtual std::string layer_text(Layer l) const = 0; // that one layer's stored value ("" if unset)
  virtual void clear(Layer l) = 0;
  // Parse `text` for this Kind and store it at `l`. False = unparseable, nothing stored.
  virtual bool set_text(Layer l, std::string_view text) = 0;

 protected:
  CVarBase(const char* name, Kind kind, const char* help, bool persistable, bool external);
  CVarBase(const CVarBase&) = delete;
  CVarBase& operator=(const CVarBase&) = delete;

  // Pull the PSXPORT_* environment variable of this name into the Override layer, once. Lazy on
  // purpose: there is no init call to forget, and a test may set the variable after startup.
  void ensure_env_bound() const;
  virtual void bind_env() = 0;

  const char* mName;
  const char* mHelp;
  Kind mKind;
  bool mPersistable;
  bool mExternal;
  mutable bool mEnvBound = false;
  bool mSet[kLayerCount] = {true, false, false, false};   // slot 0 (Default) always present

  friend void reset_for_test();
  friend void detail::registry_add(CVarBase*);
};

template <class T>
class CVar : public CVarBase {
 public:
  CVar(const char* name, T dflt, const char* help, bool persistable = true, bool external = false)
      : CVarBase(name, kind_of(), help, persistable, external), mDefault(std::move(dflt)) {}

  // The effective value. Binds the environment on first use.
  const T& get() const {
    ensure_env_bound();
    for (int i = kLayerCount - 1; i >= 1; --i)
      if (mSlot[i - 1].has_value()) return *mSlot[i - 1];
    return mDefault;
  }
  operator const T&() const { return get(); }

  const T& default_value() const { return mDefault; }

  // What the settings file must keep: Default or Value only. An Override is a launch argument and a
  // Runtime value is a console command; persisting either would turn a one-run choice into the
  // user's saved configuration. Straight from Dusklight's getValueForSave.
  const T& value_for_save() const {
    ensure_env_bound();
    return mSlot[(int)Layer::Value - 1].has_value() ? *mSlot[(int)Layer::Value - 1] : mDefault;
  }

  void set(Layer l, T v) {
    ensure_env_bound();
    detail::registry_lock();
    if (l == Layer::Default) {
      mDefault = std::move(v);
    } else {
      mSlot[(int)l - 1] = std::move(v);
      mSet[(int)l] = true;
    }
    detail::registry_unlock();
  }

  void clear(Layer l) override {
    if (l == Layer::Default) return;   // a CVar always has a default
    detail::registry_lock();
    mSlot[(int)l - 1].reset();
    mSet[(int)l] = false;
    detail::registry_unlock();
  }

  std::string value_text() const override { return to_text(get()); }

  std::string layer_text(Layer l) const override {
    if (l == Layer::Default) return to_text(mDefault);
    return mSlot[(int)l - 1].has_value() ? to_text(*mSlot[(int)l - 1]) : std::string();
  }

  bool set_text(Layer l, std::string_view text) override;

 protected:
  void bind_env() override;

 private:
  static constexpr Kind kind_of();
  static std::string to_text(const T& v);

  T mDefault;
  std::optional<T> mSlot[kLayerCount - 1];   // Value, Override, Runtime
};

using BoolVar = CVar<bool>;
using IntVar = CVar<long>;
using TextVar = CVar<std::string>;

// Only three instantiations exist and all three live in config.cpp. The specialisations have to be
// DECLARED here, before the extern-template lines below: declaring `extern BoolVar cv_oracle;` in
// config_vars.h instantiates enough of the class that a specialisation appearing only in the .cpp is
// "a specialisation after instantiation" and is ill-formed.
template <> constexpr Kind CVar<bool>::kind_of() { return Kind::Bool; }
template <> constexpr Kind CVar<long>::kind_of() { return Kind::Int; }
template <> constexpr Kind CVar<std::string>::kind_of() { return Kind::Text; }

template <> std::string CVar<bool>::to_text(const bool& v);
template <> std::string CVar<long>::to_text(const long& v);
template <> std::string CVar<std::string>::to_text(const std::string& v);

template <> bool CVar<bool>::set_text(Layer l, std::string_view text);
template <> bool CVar<long>::set_text(Layer l, std::string_view text);
template <> bool CVar<std::string>::set_text(Layer l, std::string_view text);

template <> void CVar<bool>::bind_env();
template <> void CVar<long>::bind_env();
template <> void CVar<std::string>::bind_env();

extern template class CVar<bool>;
extern template class CVar<long>;
extern template class CVar<std::string>;

}  // namespace psx::config

#endif  // PSXPORT_CONFIG_VAR_H

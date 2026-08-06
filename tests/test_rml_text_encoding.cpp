// test_rml_text_encoding — the overlay must put DATA into the DOM as data, and the shipped RML
// assets may only use entity references RmlUi can actually decode.
//
// THE BUG THIS WAS WRITTEN RED AGAINST (user screenshot, 2026-08-06): the debug overlay printed
//
//     render 1398x720 &middot; window 1536x790 &middot; internal 3x
//     pos X 13029 Y -2872 Z 7161 &middot; stage GAME (0x8010637C)
//
// i.e. the literal text "&middot;" where a "·" separator was intended.
//
// ROOT CAUSE — two distinct defects that share one cause, "we assumed RmlUi speaks HTML entities":
//
//   (1) RmlUi decodes only the FOUR XML predefined named entities plus numeric character
//       references. `StringUtilities::DecodeRml` (vendor/rmlui/Source/Core/StringUtilities.cpp)
//       handles `&lt; &gt; &amp; &quot;` and `&#NNN;` / `&#xHH;` — nothing else. HTML4 names like
//       `&middot;` and `&mdash;` are NOT in it, and `ElementText::BuildToken`'s second, smaller
//       decoder (lt/gt/amp/quot/nbsp) does not know them either, so they reach the font as
//       literal characters. This is upstream RmlUi's deliberate design (RML is XML-ish, not HTML),
//       not an RmlUi bug, so the fix belongs in OUR assets, not in the vendored library.
//
//   (2) `rmlui_overlay.cpp` built each readout with snprintf and handed the result to
//       `SetInnerRML`, which PARSES ITS ARGUMENT AS MARKUP. Numbers and a stage name are DATA;
//       feeding them to a markup parser is the reason an entity could appear in them at all. The
//       same confusion silently broke the "only rewrite when it changed" guard: `GetInnerRML()`
//       returns `EncodeRml(text)` (ElementText.cpp:443), so for any string containing `& < > "`
//       the comparison `GetInnerRML() != raw_markup` was ALWAYS true and the element was reparsed
//       and relaid-out every single frame.
//
// So the unit under test is the DATA->MARKUP boundary: rml_text_markup() in runtime/recomp/
// rml_text.h. Encoding there makes both defects structurally impossible — a data string can no
// longer be read as markup, and the round trip through the DOM is stable so the guard works.
//
// HERMETIC: no disc, no GPU, no window, no RmlUi context. rml_text_markup() and Rml's own
// DecodeRml are pure string functions; the asset lint reads files from the checkout.
//
// NEGATIVE RESULT DISCIPLINE: the asset lint prints its DENOMINATOR (files scanned, entity
// references found, split by kind) and FAILS if it scanned nothing, so "no unsupported entities"
// can never be confused with "I never looked". test_lint_selftest feeds it a string that MUST be
// flagged, so a lint that silently stopped flagging is a red test rather than a green one.

#include "testutil.h"

#include "rml_text.h"
#include <RmlUi/Core/StringUtilities.h>

#include <string>
#include <vector>
#include <fstream>
#include <sstream>

// ---- corpus location ---------------------------------------------------------------------------
// __FILE__ is the absolute path CMake passed the compiler (tests/CMakeLists.txt globs absolute
// paths), so <this file>/../.. is the psxport checkout under test. Same idiom as
// test_sync_submodules.cpp. If the layout ever moves, the reads below fail loudly with the path
// they computed rather than quietly scanning nothing.
static std::string repo_root() {
  std::string f = __FILE__;
  size_t slash = f.find_last_of('/');            // .../tests/test_rml_text_encoding.cpp
  std::string tests_dir = f.substr(0, slash);    // .../tests
  return tests_dir.substr(0, tests_dir.find_last_of('/'));
}

static bool read_file(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

// ---- the entity lint ---------------------------------------------------------------------------
// The exact set RmlUi can decode, read off vendor/rmlui/Source/Core/StringUtilities.cpp
// (DecodeRml) and vendor/rmlui/Source/Core/ElementText.cpp (BuildToken). Keep this list in step
// with the vendored library; it is the whole contract this test enforces.
static bool entity_is_supported(const std::string& name) {
  if (name.size() > 1 && name[0] == '#') return true;   // numeric character reference
  return name == "lt" || name == "gt" || name == "amp" || name == "quot" || name == "nbsp";
}

struct EntityScan {
  int total = 0;          // every `&...;` reference seen
  int numeric = 0;        // &#NNN; / &#xHH;  — decoded by DecodeRml
  int named_ok = 0;       // &lt; &gt; &amp; &quot; &nbsp;
  std::vector<std::string> unsupported;   // "<file>:<line>: &middot;"
};

// Scan one blob. `label` is what a hit is reported against. Entity names are letters/digits, or a
// leading '#' for a numeric reference; anything else is a bare '&' and not a reference at all.
static void scan_entities(const std::string& label, const std::string& text, EntityScan& s) {
  int line = 1;
  for (size_t i = 0; i < text.size(); i++) {
    if (text[i] == '\n') { line++; continue; }
    if (text[i] != '&') continue;
    size_t j = i + 1;
    if (j < text.size() && text[j] == '#') j++;
    while (j < text.size() && (isalnum((unsigned char)text[j]))) j++;
    if (j >= text.size() || text[j] != ';' || j == i + 1) continue;   // not a reference
    std::string name = text.substr(i + 1, j - (i + 1));
    s.total++;
    if (!entity_is_supported(name)) {
      s.unsupported.push_back(label + ":" + std::to_string(line) + ": &" + name + ";");
    } else if (name[0] == '#') {
      s.numeric++;
    } else {
      s.named_ok++;
    }
    i = j;
  }
}

// ---- 1. the data->markup boundary ---------------------------------------------------------------
static void test_markup_escapes_every_markup_character(void) {
  // The four characters RmlUi's parser treats specially must come back out as references, so a
  // data string can never be read as markup.
  CHECK_STREQ(rml_text_markup("a & b").c_str(), "a &amp; b");
  CHECK_STREQ(rml_text_markup("<b>").c_str(), "&lt;b&gt;");
  CHECK_STREQ(rml_text_markup("say \"hi\"").c_str(), "say &quot;hi&quot;");
  // The exact string from the user's screenshot, as DATA. The separator is a real U+00B7 byte
  // pair; UTF-8 passes through untouched, which is the point — no entity is involved at all.
  CHECK_STREQ(rml_text_markup("internal 3x \xc2\xb7 render").c_str(), "internal 3x \xc2\xb7 render");
  // And the literal the bug produced: if a "&middot;" ever reaches the boundary again it is shown
  // as those eight characters ON PURPOSE, escaped, rather than silently surviving as fake markup.
  CHECK_STREQ(rml_text_markup("&middot;").c_str(), "&amp;middot;");
}

static void test_markup_round_trips_through_rmlui(void) {
  // This is what makes rmlui_overlay's "only rewrite when it changed" guard work. SetInnerRML(m)
  // stores DecodeRml(m) on the text element; GetInnerRML() returns EncodeRml(that). So the guard
  // is stable exactly when DecodeRml(rml_text_markup(t)) == t for every t we set.
  static const char* const cases[] = {
    "render 1398x720 \xc2\xb7 window 1536x790 \xc2\xb7 internal 3x",
    "pos X 13029 Y -2872 Z 7161 \xc2\xb7 stage GAME (0x8010637C)",
    "playing: Song 7 (field)",
    "a & b < c > d \" e",
    "&middot;",
    "",
  };
  for (const char* t : cases) {
    const std::string markup = rml_text_markup(t);
    CHECK_STREQ(Rml::StringUtilities::DecodeRml(markup).c_str(), t);
    // Stability: re-encoding what the DOM would hand back yields the identical markup, so the
    // guard compares equal on the second frame instead of rewriting forever.
    CHECK_STREQ(Rml::StringUtilities::EncodeRml(Rml::StringUtilities::DecodeRml(markup)).c_str(),
                 markup.c_str());
  }
}

// ---- 1b. THE NEGATIVE CONTROL ---------------------------------------------------------------------
// The two strings from the user's screenshot, put through the SAME function the document parser
// applies to text (Factory.cpp:411 does `text = StringUtilities::DecodeRml(text)` before handing it
// to the text element) — so this measures RmlUi's real mechanism, not a model of it.
//
// The OLD input is what the overlay used to hand to SetInnerRML. It must still produce the FAILING
// answer here, otherwise this file could not have shown the bug and proves nothing about the fix.
static void test_old_separator_still_reproduces_the_bug(void) {
  // BEFORE — markup with an HTML entity name. RmlUi passes it through untouched, so the DOM holds
  // the literal characters and the font draws "&middot;". This is the reported symptom.
  const char* kOldVideo = "render 1398x720 &middot; window 1536x790 &middot; internal 3x";
  const std::string old_dom = Rml::StringUtilities::DecodeRml(kOldVideo);
  CHECK_STREQ(old_dom.c_str(), "render 1398x720 &middot; window 1536x790 &middot; internal 3x");
  CHECK(old_dom.find("&middot;") != std::string::npos);   // the literal survives: the bug

  // AFTER — the same line composed as DATA with a real U+00B7 and encoded at the boundary. The DOM
  // holds the character itself, and there is no entity anywhere for RmlUi to fail to decode.
  const char* kNewVideo = "render 1398x720 \xc2\xb7 window 1536x790 \xc2\xb7 internal 3x";
  const std::string new_dom = Rml::StringUtilities::DecodeRml(rml_text_markup(kNewVideo));
  CHECK_STREQ(new_dom.c_str(), kNewVideo);
  CHECK(new_dom.find("&middot;") == std::string::npos);
  CHECK(new_dom.find("&") == std::string::npos);          // no entity of any kind reaches the font

  // Same for the world readout line.
  const char* kOldWorld = "pos X 13029 Y -2872 Z 7161 &middot; stage GAME (0x8010637C)";
  CHECK(Rml::StringUtilities::DecodeRml(kOldWorld).find("&middot;") != std::string::npos);
  const char* kNewWorld = "pos X 13029 Y -2872 Z 7161 \xc2\xb7 stage GAME (0x8010637C)";
  CHECK_STREQ(Rml::StringUtilities::DecodeRml(rml_text_markup(kNewWorld)).c_str(), kNewWorld);

  // THE SECOND DEFECT, measured rather than argued: the old "only rewrite when it changed" guard
  // was `GetInnerRML() != raw_string`. GetInnerRML() is EncodeRml(stored_text), so reproduce it —
  // and it does NOT equal the raw string, which is why the comparison was true on every frame and
  // the element was reparsed and relaid-out forever.
  const std::string old_get_inner =
      Rml::StringUtilities::EncodeRml(Rml::StringUtilities::DecodeRml(kOldVideo));
  CHECK(old_get_inner != std::string(kOldVideo));                 // guard could never fire
  CHECK_STREQ(old_get_inner.c_str(),
              "render 1398x720 &amp;middot; window 1536x790 &amp;middot; internal 3x");
  // With the fix the two sides are the same string, so the guard fires and the rewrite stops.
  const std::string new_markup = rml_text_markup(kNewVideo);
  CHECK_STREQ(Rml::StringUtilities::EncodeRml(Rml::StringUtilities::DecodeRml(new_markup)).c_str(),
              new_markup.c_str());
}

// ---- 2. the lint's own self-test -----------------------------------------------------------------
static void test_lint_selftest(void) {
  // A lint that has quietly stopped flagging looks exactly like a clean corpus. Feed it one of
  // each class and assert it sorts them correctly.
  EntityScan s;
  scan_entities("selftest", "ok &amp; &#183; &#xB7; &nbsp; bad &middot; &mdash; plain & text", s);
  CHECK_EQ(s.total, 6);
  CHECK_EQ(s.numeric, 2);
  CHECK_EQ(s.named_ok, 2);
  CHECK_EQ((int)s.unsupported.size(), 2);
  CHECK(s.unsupported[0].find("&middot;") != std::string::npos);
  CHECK(s.unsupported[1].find("&mdash;") != std::string::npos);
}

// ---- 3. the shipped corpus ------------------------------------------------------------------------
static void test_shipped_rml_assets_use_only_decodable_entities(void) {
  static const char* const files[] = {
    "assets/rml/menu.rml",
    "assets/rml/rml.rcss",
    "assets/rml/menu.rcss",
  };
  EntityScan s;
  int scanned = 0;
  for (const char* rel : files) {
    std::string blob;
    const std::string path = repo_root() + "/" + rel;
    // REFUSE rather than return empty: a missing corpus must fail, not read as "clean".
    if (!read_file(path, blob)) {
      PT_FAILED("asset not found, so this test scanned NOTHING: %s", path.c_str());
      continue;
    }
    scanned++;
    scan_entities(rel, blob, s);
  }
  CHECK_EQ(scanned, (int)(sizeof(files) / sizeof(files[0])));
  fprintf(stderr, "    [lint] scanned %d files, %d entity refs (%d numeric, %d supported-named), "
                  "%d UNSUPPORTED\n",
          scanned, s.total, s.numeric, s.named_ok, (int)s.unsupported.size());
  for (const std::string& hit : s.unsupported)
    fprintf(stderr, "    [lint]   RmlUi cannot decode: %s\n", hit.c_str());
  // Denominator: the corpus is known to contain entity references, so a zero here would mean the
  // scanner broke rather than that the assets are clean.
  CHECK(s.total > 0);
  CHECK_EQ((int)s.unsupported.size(), 0);
}

// ---- 4. the structural regression gate ------------------------------------------------------------
static void test_overlay_routes_all_text_through_one_boundary(void) {
  // The defect was five hand-built markup strings. There must be exactly ONE raw SetInnerRML call
  // in the overlay — inside set_text(), the encoding boundary — so a future readout cannot
  // reintroduce data-as-markup without this test going red.
  std::string src;
  const std::string path = repo_root() + "/runtime/recomp/rmlui_overlay.cpp";
  if (!read_file(path, src)) {
    PT_FAILED("overlay source not found, so this test checked NOTHING: %s", path.c_str());
    return;
  }
  int calls = 0;
  for (size_t p = src.find("SetInnerRML("); p != std::string::npos; p = src.find("SetInnerRML(", p + 1))
    calls++;
  fprintf(stderr, "    [lint] rmlui_overlay.cpp: %d SetInnerRML( call site(s)\n", calls);
  CHECK_EQ(calls, 1);
}

int main(void) {
  RUN(markup_escapes_every_markup_character);
  RUN(markup_round_trips_through_rmlui);
  RUN(old_separator_still_reproduces_the_bug);
  RUN(lint_selftest);
  RUN(shipped_rml_assets_use_only_decodable_entities);
  RUN(overlay_routes_all_text_through_one_boundary);
  return pt_summary();
}

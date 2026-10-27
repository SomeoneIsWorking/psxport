#include "owner_journal.h"

#include "config_vars.h"
#include "core.h"
#include "fs_util.h"
#include "game.h"
#include "image_identity.h"
#include "title_debug_menu.h"

#include <lucent/log.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <vector>

namespace psx::present {
namespace {

std::uint32_t writerCode(const GuestWriter &writer) {
  return writer.function != 0 ? writer.function : writer.blockPc;
}

bool sameRow(const OwnerRow &row, const OwnerFinding &finding, std::size_t screen, const GuestWriter &writer) {
  const OwnerFinding &key = row.key;
  return row.screen == screen && key.reason == finding.reason && key.cause == finding.cause &&
         key.producer == finding.producer && key.table == finding.table && row.writer.kind == writer.kind &&
         writerCode(row.writer) == writerCode(writer) && row.writer.dispatched == writer.dispatched;
}

std::string hex(std::uint32_t value) {
  return lucent::format("0x{:08X}", value);
}

// A JSON string literal; rows hold only printable ASCII from the framework and title names.
std::string jsonString(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
    }
    out += c;
  }
  out += '"';
  return out;
}

} // namespace

const char *ownerReasonName(OwnerReason reason) {
  switch (reason) {
  case OwnerReason::UnownedMoving:
    return "UnownedMoving";
  case OwnerReason::Port:
    return "Port";
  case OwnerReason::Baked:
    return "Baked";
  case OwnerReason::Overwrote:
    return "Overwrote";
  case OwnerReason::ReplayRefused:
    return "ReplayRefused";
  case OwnerReason::IntervalHeld:
    return "IntervalHeld";
  }
  return "?";
}

const char *heldCauseName(HeldCause cause) {
  switch (cause) {
  case HeldCause::None:
    return "";
  case HeldCause::Cut:
    return "cut";
  case HeldCause::StaleShown:
    return "stale-shown";
  case HeldCause::NoPrevious:
    return "no-previous";
  case HeldCause::ChainBreak:
    return "chain-break";
  case HeldCause::NoState:
    return "no-state";
  case HeldCause::ProducerRefused:
    return "producer-refused";
  case HeldCause::Unplaceable:
    return "unplaceable";
  case HeldCause::Unpaired:
    return "unpaired";
  }
  return "?";
}

OwnerJournal::OwnerJournal() {
  rows_.reserve(kMaxRows);
  screens_.reserve(kMaxScreens);
  const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm local{};
  localtime_r(&now, &local);
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &local);
  stamp_ = buffer;
}

void OwnerJournal::observeScreen(Core &core, std::uint64_t key) {
  const psx::game::TitleDebugMenu &menu = core.runtime.debugMenu();
  if (screenKnown_ && screens_[screen_].key == key) {
    return;
  }
  if (screenKnown_ && !rows_.empty()) {
    flush(core);
  }
  screenKnown_ = true;
  const auto known = std::find_if(screens_.begin(), screens_.end(), [key](const Screen &screen) {
    return screen.key == key;
  });
  if (known != screens_.end()) {
    screen_ = static_cast<std::size_t>(known - screens_.begin());
    return;
  }
  if (screens_.size() == kMaxScreens) {
    screen_ = 0;
    return;
  }
  screens_.push_back({key, menu.screenRoute(core, key)});
  screen_ = screens_.size() - 1u;
  lucent::info("owners",
               "screen {} (key 0x{:X}): {}",
               screen_,
               key,
               screens_[screen_].route.empty() ? "(no route)" : screens_[screen_].route);
}

void OwnerJournal::record(Core &core, const OwnerFinding &finding, bool lost) {
  if (!screenKnown_) {
    observeScreen(core, core.runtime.debugMenu().screenKey(core));
  }
  // A finding with no writer names the declared owner's function, the code that drew the object.
  GuestWriter writer;
  if (const GuestWriter *known = core.owners.writer(finding.writer)) {
    writer = *known;
  } else if (finding.producer != 0) {
    writer.function = finding.producer;
    writer.image = core.imageCatalog().resolve(finding.producer).value_or(cpu::ImageIdentity{});
  }
  auto row = std::find_if(rows_.begin(), rows_.end(), [&](const OwnerRow &candidate) {
    return sameRow(candidate, finding, screen_, writer);
  });
  const auto field = static_cast<std::uint32_t>(core.game->timing.displayField());
  const std::uint64_t present = core.game->presentation.fence();
  if (row == rows_.end()) {
    if (rows_.size() == kMaxRows) {
      if (!fullReported_) {
        fullReported_ = true;
        lucent::warn("owners", "{} rows held; later findings are dropped", kMaxRows);
      }
      return;
    }
    OwnerRow added{.key = finding, .writer = writer, .screen = screen_, .firstField = field, .firstPresent = present};
    rows_.push_back(added);
    row = rows_.end() - 1;
    lucent::info("owners",
                 "new row: {}{}{} {} fn={} block={} dispatched={} producer={} ot={}:{}-{} packet={} cmd=0x{:02X} "
                 "screen={}",
                 ownerReasonName(finding.reason),
                 finding.cause != HeldCause::None ? "/" : "",
                 heldCauseName(finding.cause),
                 writerKindName(row->writer.kind),
                 hex(row->writer.function),
                 hex(row->writer.blockPc),
                 hex(row->writer.dispatched),
                 hex(finding.producer),
                 finding.table == kNoTable ? -1 : static_cast<int>(finding.table),
                 finding.bucketLo,
                 finding.bucketHi,
                 hex(finding.sampleAddress),
                 finding.sampleCommand,
                 screens_.empty() ? std::string{} : screens_[screen_].route);
  }
  ++row->intervals;
  row->intervalsLost += lost ? 1u : 0u;
  row->primitives += finding.primitives;
  row->maxPrimitives = std::max(row->maxPrimitives, finding.primitives);
  row->key.bucketLo = std::min(row->key.bucketLo, finding.bucketLo);
  row->key.bucketHi = std::max(row->key.bucketHi, finding.bucketHi);
  row->lastField = field;
  row->lastPresent = present;
}

std::string OwnerJournal::describe(std::size_t limit) const {
  std::vector<const OwnerRow *> order;
  order.reserve(rows_.size());
  for (const OwnerRow &row : rows_) {
    order.push_back(&row);
  }
  std::stable_sort(order.begin(), order.end(), [](const OwnerRow *a, const OwnerRow *b) {
    return a->intervalsLost > b->intervalsLost;
  });
  std::string out = lucent::format("{} row(s), most intervals lost first\n", rows_.size());
  for (std::size_t i = 0; i < order.size() && i < limit; ++i) {
    const OwnerRow &row = *order[i];
    out += lucent::format("{:>6} lost {:>6} seen {}{}{} {} fn={} block={} dispatched={} producer={} ot={}:{}-{} "
                          "prims/interval<={} screen='{}'\n",
                          row.intervalsLost,
                          row.intervals,
                          ownerReasonName(row.key.reason),
                          row.key.cause != HeldCause::None ? "/" : "",
                          heldCauseName(row.key.cause),
                          writerKindName(row.writer.kind),
                          hex(row.writer.function),
                          hex(row.writer.blockPc),
                          hex(row.writer.dispatched),
                          hex(row.key.producer),
                          row.key.table == kNoTable ? -1 : static_cast<int>(row.key.table),
                          row.key.bucketLo,
                          row.key.bucketHi,
                          row.maxPrimitives,
                          row.screen < screens_.size() ? screens_[row.screen].route : std::string{});
  }
  return out;
}

std::string OwnerJournal::decompCommand(Core &core, const OwnerRow &row) const {
  const psx::host::TitleIdentity *title = core.game->title;
  const std::optional<cpu::ImageDescription> described = core.imageCatalog().describe(row.writer.image);
  if (title == nullptr || !described || writerCode(row.writer) == 0) {
    return {};
  }
  const bool resident =
      !described->ranges.empty() && described->ranges.front().begin == (title->textAddress & 0x1FFFFFFFu);
  const std::optional<psx::game::DecompImage> image =
      core.runtime.debugMenu().decompImage(core, described->name, resident);
  if (!image) {
    return {};
  }
  std::error_code error;
  const std::filesystem::path cwd = std::filesystem::current_path(error);
  const std::filesystem::path file = error ? image->file : image->file.lexically_proximate(cwd);
  // The function is decompiled whole; the block PC is asked only when it is a label inside some function.
  std::string queries;
  if (row.writer.function != 0) {
    queries += " --target " + hex(row.writer.function);
  }
  if (row.writer.blockPc != 0 && row.writer.blockPc != row.writer.function) {
    queries += " --function-at " + hex(row.writer.blockPc);
  }
  return lucent::format("uv run --frozen python external/psxport/tools/decomp_pipeline.py --image-name {} --image {}{} "
                        "--out scratch/decomp/{}",
                        image->manifestName,
                        file.string(),
                        queries,
                        image->manifestName);
}

std::string OwnerJournal::rowJson(Core &core, const OwnerRow &row) const {
  const Game &game = *core.game;
  const psx::host::TitleIdentity *title = game.title;
  std::string image;
  if (const std::optional<cpu::ImageDescription> described = core.imageCatalog().describe(row.writer.image)) {
    image = described->name;
  }
  const std::string decomp = decompCommand(core, row);
  const std::string &route = row.screen < screens_.size() ? screens_[row.screen].route : std::string{};
  return lucent::format(
      "{{\"title\":{},\"serial\":{},\"reason\":{},\"cause\":{},\"writer_kind\":{},\"function\":{},\"block_pc\":{},"
      "\"dispatched\":{},\"image\":{},\"image_generation\":{},\"producer\":{},\"ot_table\":{},\"ot_buckets\":[{},{}],"
      "\"sample_packet\":{},\"gp0_command\":{},\"intervals\":{},\"intervals_lost\":{},\"primitives_total\":{},"
      "\"primitives_max_per_interval\":{},\"fields\":[{},{}],\"presents\":[{},{}],\"screen\":{},\"screen_key\":{},"
      "\"warp\":{},"
      "\"decomp\":{}}}",
      jsonString(title != nullptr ? title->displayName : std::string_view{}),
      jsonString(title != nullptr ? title->serial : std::string_view{}),
      jsonString(ownerReasonName(row.key.reason)),
      jsonString(heldCauseName(row.key.cause)),
      jsonString(writerKindName(row.writer.kind)),
      jsonString(hex(row.writer.function)),
      jsonString(hex(row.writer.blockPc)),
      jsonString(hex(row.writer.dispatched)),
      jsonString(image),
      row.writer.image.generation,
      jsonString(hex(row.key.producer)),
      row.key.table == kNoTable ? -1 : static_cast<int>(row.key.table),
      row.key.bucketLo,
      row.key.bucketHi,
      jsonString(hex(row.key.sampleAddress)),
      jsonString(lucent::format("0x{:02X}", row.key.sampleCommand)),
      row.intervals,
      row.intervalsLost,
      row.primitives,
      row.maxPrimitives,
      row.firstField,
      row.lastField,
      row.firstPresent,
      row.lastPresent,
      row.screen,
      jsonString(lucent::format("0x{:X}", row.screen < screens_.size() ? screens_[row.screen].key : 0u)),
      jsonString(route),
      decomp.empty() ? std::string("null") : jsonString(decomp));
}

bool OwnerJournal::writeJsonl(Core &core, const std::string &path) const {
  if (!Fs::ensureParentDirs(path.c_str())) {
    return false;
  }
  std::ofstream out(path, std::ios::trunc);
  for (const OwnerRow &row : rows_) {
    out << rowJson(core, row) << '\n';
  }
  return static_cast<bool>(out);
}

std::string OwnerJournal::path(Core &core) const {
  const std::string dir = psx::config::owners_dir();
  if (dir.empty()) {
    return {};
  }
  const psx::host::TitleIdentity *title = core.game->title;
  return lucent::format("{}/{}-{}.jsonl", dir, title != nullptr ? title->slug : std::string_view{"selector"}, stamp_);
}

void OwnerJournal::flush(Core &core) const {
  const std::string file = path(core);
  if (file.empty() || rows_.empty()) {
    return;
  }
  if (!writeJsonl(core, file)) {
    lucent::warn("owners", "could not write {}", file);
    return;
  }
  lucent::info("owners", "{} row(s) across {} screen(s) written to {}", rows_.size(), screens_.size(), file);
}

} // namespace psx::present

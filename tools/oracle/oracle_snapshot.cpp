#include "oracle_snapshot.h"

#include "mednafen-types.h" // Its C++ standard-library includes require C++ linkage.

extern "C" {
#include "cpu.h"
#include "dma_dpcr.h"
#include "irq.h"
#include "oracle_shim.h"
#include "psx.h"
}
#include "gte_state.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr std::array<unsigned char, 8> kMagic{'O', 'R', 'A', 'S', 'N', 'A', 'P', '1'};
constexpr uint32_t kVersion = 1;
constexpr size_t kMaxBytes = 3u * 1024u * 1024u;
constexpr size_t kMaxFields = 64;

struct Field {
  std::string name;
  unsigned char *data;
  size_t bytes;
  unsigned width;
};

struct Fields {
  std::vector<Field> entries;
  bool valid = true;
  uint32_t dpcr = 0;

  void add(const char *name, void *data, size_t bytes, unsigned width) {
    if (!name || !data || !bytes || bytes > kMaxBytes || (width != 1 && width != 2 && width != 4 && width != 8) ||
        bytes % width || entries.size() == kMaxFields || std::strlen(name) > 95 ||
        std::any_of(entries.begin(), entries.end(), [&](const Field &field) {
          return field.name == name;
        })) {
      std::fprintf(stderr, "oracle snapshot: invalid/duplicate owner field %s\n", name ? name : "(null)");
      valid = false;
      return;
    }
    entries.push_back({name, static_cast<unsigned char *>(data), bytes, width});
  }
};

Fields *active_fields = nullptr;

void collect_shim(void *context, const char *name, void *data, size_t bytes, unsigned width) {
  static_cast<Fields *>(context)->add(name, data, bytes, width);
}

bool collect(Fields &fields) {
  if (!oracle_main_ram() || active_fields) {
    std::fprintf(stderr, "oracle snapshot: oracle is not initialized or collection is already active\n");
    return false;
  }
  active_fields = &fields;
  // Save-direction callbacks expose CPU/IRQ-owned storage, including cpu.c's private fields.
  // Loading through the compatibility savestate reader would normalize pending-load/register bits.
  const bool owners_ok = CPU_StateAction(PSX_CPU, nullptr, 0, false) && IRQ_StateAction(nullptr, 0, 0);
  active_fields = nullptr;

  fields.add("CPU.extra/load_dummy", &PSX_CPU->GPR_full[34], sizeof(uint32_t), 4);
  fields.add("CPU.extra/read_absorb_tail", &PSX_CPU->ReadAbsorb[33], 2, 1);
  fields.add("CPU.extra/address_masks", PSX_CPU->addr_mask, sizeof(PSX_CPU->addr_mask), 4);
  fields.add("CPU.extra/dummy_page", PSX_CPU->DummyPage, sizeof(PSX_CPU->DummyPage), 1);
  GteRegs *gte = GTE_CurState();
  fields.add("GTE/registers", gte->REG, sizeof(gte->REG), 4);
  fields.add("GTE/flags", &gte->FLAGS, sizeof(gte->FLAGS), 4);
  fields.add("memory/main_ram", oracle_main_ram(), oracle_ram_size(), 1);
  fields.dpcr = DMA_DPCR_SaveStateValue();
  fields.add("DMA/DPCR", &fields.dpcr, sizeof(fields.dpcr), 4);
  return owners_ok && oracle_snapshot_visit_shim(collect_shim, &fields, 0) && fields.valid;
}

void append32(std::vector<unsigned char> &out, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<unsigned char>(value >> shift));
  }
}

uint32_t read32(const unsigned char *bytes) {
  return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}

uint32_t checksum(const unsigned char *data, size_t size) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < size; ++i) {
    hash = (hash ^ data[i]) * 16777619u;
  }
  return hash;
}

void transfer(unsigned char *destination, const unsigned char *source, size_t bytes, unsigned width) {
  if constexpr (std::endian::native == std::endian::little) {
    std::memcpy(destination, source, bytes);
  } else {
    for (size_t base = 0; base < bytes; base += width) {
      for (unsigned lane = 0; lane < width; ++lane) {
        destination[base + lane] = source[base + width - lane - 1];
      }
    }
  }
}

std::vector<unsigned char> encode(const Fields &fields) {
  std::vector<unsigned char> out(kMagic.begin(), kMagic.end());
  append32(out, kVersion);
  append32(out, static_cast<uint32_t>(fields.entries.size()));
  append32(out, 0);
  append32(out, 0);
  for (const Field &field : fields.entries) {
    append32(out, static_cast<uint32_t>(field.name.size()));
    append32(out, static_cast<uint32_t>(field.bytes));
    append32(out, field.width);
    out.insert(out.end(), field.name.begin(), field.name.end());
    const size_t start = out.size();
    out.resize(start + field.bytes);
    transfer(out.data() + start, field.data, field.bytes, field.width);
  }
  const uint32_t size = static_cast<uint32_t>(out.size());
  const uint32_t hash = checksum(out.data() + 24, out.size() - 24);
  for (unsigned lane = 0; lane < 4; ++lane) {
    out[16 + lane] = static_cast<unsigned char>(size >> (lane * 8));
    out[20 + lane] = static_cast<unsigned char>(hash >> (lane * 8));
  }
  return out;
}

bool state_value_valid(const Field &field, const unsigned char *data) {
  const uint32_t value = field.bytes >= 4 ? read32(data) : data[0];
  if (field.name == "CPU/BACKED_LDWhich") {
    return value <= 31 || value == 34;
  }
  if (field.name == "CPU/ReadAbsorbWhich") {
    return value <= 31;
  }
  if (field.name == "CPU/ReadFudge") {
    return value < 35;
  }
  if (field.name == "CPU/BDBT") {
    return value == 0 || value == 2 || value == 3;
  }
  if (field.name == "CPU/Halted") {
    return value <= 1;
  }
  if (field.name == "shim/timestamp") {
    return value <= INT32_MAX;
  }
  if (field.name == "shim/stop" || field.name == "shim/taint") {
    return value <= ORACLE_STOP_EVENT;
  }
  if (field.name == "shim/device_writes") {
    return (value & ~ORACLE_DEVICE_ALL) == 0;
  }
  return true;
}

bool decode(const std::vector<unsigned char> &in, const Fields &fields, std::vector<size_t> &offsets) {
  if (in.size() < 24 || in.size() > kMaxBytes || !std::equal(kMagic.begin(), kMagic.end(), in.begin()) ||
      read32(in.data() + 8) != kVersion || read32(in.data() + 12) != fields.entries.size() ||
      read32(in.data() + 16) != in.size() || read32(in.data() + 20) != checksum(in.data() + 24, in.size() - 24)) {
    std::fprintf(stderr, "oracle snapshot: invalid magic/version/length/field-count/checksum\n");
    return false;
  }
  size_t cursor = 24;
  for (const Field &field : fields.entries) {
    if (in.size() - cursor < 12) {
      return false;
    }
    const uint32_t name_bytes = read32(in.data() + cursor);
    const uint32_t bytes = read32(in.data() + cursor + 4);
    const uint32_t width = read32(in.data() + cursor + 8);
    cursor += 12;
    if (name_bytes != field.name.size() || bytes != field.bytes || width != field.width ||
        name_bytes > in.size() - cursor || bytes > in.size() - cursor - name_bytes ||
        std::memcmp(in.data() + cursor, field.name.data(), name_bytes)) {
      std::fprintf(stderr, "oracle snapshot: schema mismatch at owner field %s\n", field.name.c_str());
      return false;
    }
    cursor += name_bytes;
    if (!state_value_valid(field, in.data() + cursor)) {
      std::fprintf(stderr, "oracle snapshot: invalid execution state in %s\n", field.name.c_str());
      return false;
    }
    offsets.push_back(cursor);
    cursor += bytes;
  }
  return cursor == in.size();
}

using File = std::unique_ptr<FILE, decltype(&std::fclose)>;

} // namespace

// CPU/GTE/IRQ implementations call this owner interface. Collect only stable CPU/IRQ storage;
// GTE's compatibility arrays are function-local and do not preserve every raw register bit.
extern "C" int MDFNSS_StateAction(void *, int load, int, SFORMAT *sf, const char *section) {
  if (!active_fields || load || !section) {
    std::fprintf(stderr, "oracle snapshot: unexpected savestate owner callback\n");
    std::abort();
  }
  if (std::strcmp(section, "GTE") == 0) {
    return 1;
  }
  if (std::strcmp(section, "CPU") != 0 && std::strcmp(section, "IRQ") != 0) {
    return 0;
  }
  for (size_t i = 0; sf[i].name; ++i) {
    const SFORMAT &source = sf[i];
    unsigned width = 1;
    if (source.flags & MDFNSTATE_BOOL) {
      width = sizeof(bool);
    } else if (source.flags & MDFNSTATE_RLSB32) {
      width = 4;
    } else if (source.flags & MDFNSTATE_RLSB16) {
      width = 2;
    } else if (source.flags & MDFNSTATE_RLSB64) {
      width = 8;
    } else if (source.flags & MDFNSTATE_RLSB) {
      width = source.size;
    }
    // These CPU-owned arrays use SFVAR in the compatibility registry; their public owner types
    // establish the element width even though the registry describes the whole array as one value.
    if (std::strcmp(section, "CPU") == 0 &&
        (std::strcmp(source.name, "ICache_Bulk") == 0 || std::strcmp(source.name, "cpu_CP0.Regs") == 0)) {
      width = 4;
    }
    const std::string name = std::string(section) + "/" + source.name;
    active_fields->add(name.c_str(), source.v, source.size, width);
  }
  return active_fields->valid;
}

extern "C" int oracle_snapshot_save(const char *path) {
  Fields fields;
  if (!path || !collect(fields)) {
    return 0;
  }
  const auto bytes = encode(fields);
  File file(std::fopen(path, "wb"), std::fclose);
  if (!file || std::fwrite(bytes.data(), 1, bytes.size(), file.get()) != bytes.size() || std::fflush(file.get())) {
    std::fprintf(stderr, "oracle snapshot: cannot write %s\n", path);
    return 0;
  }
  std::fprintf(
      stderr, "oracle snapshot: saved %zu owner fields, %zu bytes to %s\n", fields.entries.size(), bytes.size(), path);
  return 1;
}

extern "C" int oracle_snapshot_load(const char *path) {
  Fields fields;
  if (!path || !collect(fields)) {
    return 0;
  }
  File file(std::fopen(path, "rb"), std::fclose);
  if (!file) {
    std::fprintf(stderr, "oracle snapshot: cannot open %s\n", path);
    return 0;
  }
  std::vector<unsigned char> bytes;
  std::array<unsigned char, 8192> chunk{};
  size_t count;
  while ((count = std::fread(chunk.data(), 1, chunk.size(), file.get())) != 0) {
    if (count > kMaxBytes - bytes.size()) {
      std::fprintf(stderr, "oracle snapshot: file exceeds byte budget\n");
      return 0;
    }
    bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<ptrdiff_t>(count));
  }
  if (std::ferror(file.get())) {
    std::fprintf(stderr, "oracle snapshot: read failed\n");
    return 0;
  }
  std::vector<size_t> offsets;
  if (!decode(bytes, fields, offsets)) {
    return 0;
  }
  // All allocation/schema/value checks precede mutation. The pointers are supplied by the actual
  // owners, so copying restores private pipeline fields without reconstruction or normalization.
  for (size_t i = 0; i < fields.entries.size(); ++i) {
    const Field &field = fields.entries[i];
    transfer(field.data, bytes.data() + offsets[i], field.bytes, field.width);
  }
  DMA_DPCR_LoadStateValue(fields.dpcr);
  std::fprintf(stderr,
               "oracle snapshot: restored %zu owner fields, %zu bytes from %s\n",
               fields.entries.size(),
               bytes.size(),
               path);
  return 1;
}

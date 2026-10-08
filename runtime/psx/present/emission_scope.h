// emission_scope.h — which producer call is writing guest packets, and the key each packet carries.
#pragma once

#include "frame_record.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace psx::present {

// The argument register that names a producer's object.
enum class Arg : std::uint8_t { A0, A1, A2, A3 };

struct Producer {
  Arg object = Arg::A0;
};

// One per Core. While a scope (P, obj, element) is open, a store to a main-RAM word binds the word to
// that key; a store outside every scope unbinds it. Native and guest stores both count: Lightrec routes
// every RAM store through Core::mem_w*. A packet's key is the binding of its first command word, so links
// written into its header later, by anyone, leave it alone, and a binding outlives the record seal until
// the word is rewritten. Keys are identity only: a producer that writes several packets for one object names
// each with element(); packets left sharing a key are ambiguous and never interpolated. Every object scope
// gets a new serial, which its element scopes share and its packets carry: the state a producer saves in
// that scope is the state those packets were drawn from.
class EmissionScope {
public:
  EmissionScope();

  class Guard {
  public:
    Guard(EmissionScope &scope, std::uint32_t producer, std::uint32_t object, std::uint32_t element);
    ~Guard();
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;
    Guard(Guard &&) = delete;
    Guard &operator=(Guard &&) = delete;

  private:
    friend class EmissionScope;
    Guard(EmissionScope &scope, const RecordKey &key);
    EmissionScope &scope_;
  };

  // A list walker's scope for one object, under the producer of the innermost open scope.
  [[nodiscard]] Guard instance(std::uint32_t object);
  // One element of the innermost open object (a primitive index, a sub-part).
  [[nodiscard]] Guard element(std::uint32_t index);

  bool isOpen() const {
    return !stack_.empty();
  }
  std::size_t depth() const {
    return stack_.size();
  }
  // The innermost open scope's key; aborts with none open.
  const RecordKey &current() const;
  void noteStore(std::uint32_t address);
  // Names the packet at `packetAddress` by the innermost open scope, for a producer that keys packets its
  // guest body already wrote.
  void bindPacket(std::uint32_t packetAddress);
  // The key of the packet whose header word is at `packetAddress`, with part 0.
  std::optional<RecordKey> keyFor(std::uint32_t packetAddress) const;
  // Main RAM was replaced wholesale (a savestate load): nothing is bound.
  void clear();

private:
  void push(const RecordKey &key);
  void pop();
  std::vector<RecordKey> stack_;
  std::uint32_t nextSerial_ = 1;
  std::vector<RecordKey> words_; // per main-RAM word; producer 0 is unbound
};

} // namespace psx::present

// test_emission_scope.cpp — producer scopes bind the packets they store to (P, obj, element).
#include "emission_scope.h"
#include "testutil.h"

#include <optional>

namespace {

using psx::present::EmissionScope;
using psx::present::RecordKey;

constexpr std::uint32_t kProducer = 0x8001F798u;
constexpr std::uint32_t kListProducer = 0x80020000u;

bool keyIs(const std::optional<RecordKey> &key, std::uint32_t producer, std::uint32_t object, std::uint32_t element) {
  return key.has_value() && key->producer == producer && key->object == object && key->element == element &&
         key->part == 0;
}

// A packet's header word, then its first command word.
void writePacket(EmissionScope &scope, std::uint32_t packet) {
  scope.noteStore(packet);
  scope.noteStore(packet + 4u);
}

} // namespace

static void test_stores_outside_a_scope_bind_nothing(void) {
  EmissionScope scope;
  writePacket(scope, 0x80100000u);
  CHECK(!scope.isOpen());
  CHECK(!scope.keyFor(0x80100000u).has_value());
}

static void test_a_packet_is_keyed_by_its_first_command_word(void) {
  EmissionScope scope;
  {
    EmissionScope::Guard guard(scope, kProducer, 0x80150000u, 0);
    scope.noteStore(0x80100007u); // a byte store binds its word
    scope.noteStore(0x80100020u); // a header alone keys nothing
  }
  CHECK(keyIs(scope.keyFor(0x80100003u), kProducer, 0x80150000u, 0));
  // KUSEG, KSEG1 and the mirrors name the same main-RAM word.
  CHECK(keyIs(scope.keyFor(0x00100000u), kProducer, 0x80150000u, 0));
  CHECK(keyIs(scope.keyFor(0xA0300000u), kProducer, 0x80150000u, 0));
  CHECK(!scope.keyFor(0x80100020u).has_value());
}

static void test_a_link_written_into_the_header_keeps_the_key(void) {
  EmissionScope scope;
  {
    EmissionScope::Guard first(scope, kProducer, 0xA0u, 0);
    writePacket(scope, 0x80100000u);
  }
  {
    EmissionScope::Guard second(scope, kProducer, 0xB0u, 0);
    writePacket(scope, 0x80100010u);
    scope.noteStore(0x80100000u);
  }
  scope.noteStore(0x80100010u);
  CHECK(keyIs(scope.keyFor(0x80100000u), kProducer, 0xA0u, 0));
  CHECK(keyIs(scope.keyFor(0x80100010u), kProducer, 0xB0u, 0));
}

static void test_a_command_word_rewritten_outside_a_scope_is_unkeyed(void) {
  EmissionScope scope;
  {
    EmissionScope::Guard guard(scope, kProducer, 0xA0u, 0);
    writePacket(scope, 0x80100000u);
  }
  writePacket(scope, 0x80100000u);
  CHECK(!scope.keyFor(0x80100000u).has_value());
}

static void test_scratchpad_is_not_a_packet(void) {
  EmissionScope scope;
  EmissionScope::Guard guard(scope, kProducer, 1u, 0);
  writePacket(scope, 0x1F800010u);
  CHECK(!scope.keyFor(0x1F800010u).has_value());
}

static void test_nested_scopes_restore_the_outer_one(void) {
  EmissionScope scope;
  {
    EmissionScope::Guard outer(scope, kProducer, 0xA0u, 0);
    writePacket(scope, 0x80100000u);
    {
      EmissionScope::Guard inner(scope, kListProducer, 0xB0u, 0);
      CHECK_EQ(scope.depth(), 2u);
      writePacket(scope, 0x80100100u);
    }
    writePacket(scope, 0x80100200u);
  }
  CHECK_EQ(scope.depth(), 0u);
  CHECK(keyIs(scope.keyFor(0x80100000u), kProducer, 0xA0u, 0));
  CHECK(keyIs(scope.keyFor(0x80100100u), kListProducer, 0xB0u, 0));
  CHECK(keyIs(scope.keyFor(0x80100200u), kProducer, 0xA0u, 0));
}

static void test_instance_and_element_name_the_object_and_its_part(void) {
  EmissionScope scope;
  {
    EmissionScope::Guard walker(scope, kListProducer, 0x80180000u, 0);
    for (std::uint32_t object = 0; object < 3; object++) {
      auto instance = scope.instance(0x80190000u + object * 0x40u);
      for (std::uint32_t part = 0; part < 2; part++) {
        auto element = scope.element(part);
        writePacket(scope, 0x80100000u + object * 0x20u + part * 0x10u);
      }
    }
  }
  for (std::uint32_t object = 0; object < 3; object++) {
    for (std::uint32_t part = 0; part < 2; part++) {
      CHECK(keyIs(scope.keyFor(0x80100000u + object * 0x20u + part * 0x10u),
                  kListProducer,
                  0x80190000u + object * 0x40u,
                  part));
    }
  }
}

// The key does not depend on how many words were written before: culling cannot shift it.
static void test_key_is_independent_of_earlier_stores(void) {
  EmissionScope scope;
  {
    EmissionScope::Guard guard(scope, kProducer, 0xA0u, 0);
    scope.noteStore(0x801FFF00u); // a stack spill
    auto element = scope.element(7);
    writePacket(scope, 0x80100000u);
  }
  CHECK(keyIs(scope.keyFor(0x80100000u), kProducer, 0xA0u, 7));
}

static void test_a_later_scope_that_rewrites_a_packet_owns_it(void) {
  EmissionScope scope;
  {
    EmissionScope::Guard first(scope, kProducer, 0xA0u, 0);
    writePacket(scope, 0x80100010u);
  }
  {
    EmissionScope::Guard other(scope, kProducer, 0xB0u, 0);
    writePacket(scope, 0x80100010u);
  }
  CHECK(keyIs(scope.keyFor(0x80100010u), kProducer, 0xB0u, 0));
}

static void test_bind_packet_names_a_packet_the_guest_wrote(void) {
  EmissionScope scope;
  writePacket(scope, 0x80100000u);
  {
    EmissionScope::Guard guard(scope, kProducer, 0xA0u, 3);
    scope.bindPacket(0x80100000u);
  }
  CHECK(keyIs(scope.keyFor(0x80100000u), kProducer, 0xA0u, 3));
}

static void test_clear_unbinds_everything(void) {
  EmissionScope scope;
  {
    EmissionScope::Guard guard(scope, kProducer, 0xA0u, 0);
    writePacket(scope, 0x80100000u);
  }
  scope.clear();
  CHECK(!scope.keyFor(0x80100000u).has_value());
}

static void test_identity_ignores_the_scope_serial(void) {
  EmissionScope scope;
  {
    EmissionScope::Guard first(scope, kProducer, 0xA0u, 2);
    writePacket(scope, 0x80100000u);
  }
  {
    EmissionScope::Guard second(scope, kProducer, 0xA0u, 2);
    writePacket(scope, 0x80100100u);
  }
  CHECK(scope.keyFor(0x80100000u) != scope.keyFor(0x80100100u));
  CHECK(scope.identityFor(0x80100000u) == scope.identityFor(0x80100100u));
  CHECK(scope.identityFor(0x80100000u) == (RecordKey{kProducer, 0xA0u, 2u, 0u}));
  CHECK(!scope.identityFor(0x80100200u).has_value());
}

int main(void) {
  RUN(stores_outside_a_scope_bind_nothing);
  RUN(identity_ignores_the_scope_serial);
  RUN(a_packet_is_keyed_by_its_first_command_word);
  RUN(a_link_written_into_the_header_keeps_the_key);
  RUN(a_command_word_rewritten_outside_a_scope_is_unkeyed);
  RUN(scratchpad_is_not_a_packet);
  RUN(nested_scopes_restore_the_outer_one);
  RUN(instance_and_element_name_the_object_and_its_part);
  RUN(key_is_independent_of_earlier_stores);
  RUN(a_later_scope_that_rewrites_a_packet_owns_it);
  RUN(bind_packet_names_a_packet_the_guest_wrote);
  RUN(clear_unbinds_everything);
  return pt_summary();
}

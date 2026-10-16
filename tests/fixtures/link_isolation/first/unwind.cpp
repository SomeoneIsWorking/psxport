// Both trees unwind through a destructor, so PIC objects each define DW.ref.__gxx_personality_v0;
// the noexcept caller makes Clang emit __clang_call_terminate in each.
namespace link_fixture {

void firstCallee();

struct FirstGuard {
  ~FirstGuard();
};

void firstUnwinds() {
  FirstGuard guard;
  firstCallee();
}

void firstNoexcept() noexcept {
  firstCallee();
}

} // namespace link_fixture

// Both trees unwind through a destructor, so PIC objects each define DW.ref.__gxx_personality_v0;
// the noexcept caller makes Clang emit __clang_call_terminate in each.
namespace link_fixture {

void secondCallee();

struct SecondGuard {
  ~SecondGuard();
};

void secondUnwinds() {
  SecondGuard guard;
  secondCallee();
}

void secondNoexcept() noexcept {
  secondCallee();
}

} // namespace link_fixture

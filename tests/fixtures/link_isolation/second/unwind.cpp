// Both trees unwind through a destructor, so PIC objects each define DW.ref.__gxx_personality_v0.
namespace link_fixture {

void secondCallee();

struct SecondGuard {
  ~SecondGuard();
};

void secondUnwinds() {
  SecondGuard guard;
  secondCallee();
}

} // namespace link_fixture

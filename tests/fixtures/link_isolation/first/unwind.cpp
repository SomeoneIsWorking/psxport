// Both trees unwind through a destructor, so PIC objects each define DW.ref.__gxx_personality_v0.
namespace link_fixture {

void firstCallee();

struct FirstGuard {
  ~FirstGuard();
};

void firstUnwinds() {
  FirstGuard guard;
  firstCallee();
}

} // namespace link_fixture

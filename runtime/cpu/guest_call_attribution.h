#pragma once

#include <array>
#include <cstdint>

namespace psx::cpu {

// Diagnostic-only shadow of guest call ownership.  It is maintained by the
// executor/override boundary and never participates in execution decisions.
class GuestCallAttribution {
public:
  static constexpr int Capacity = 256;

  void push(std::uint32_t address) {
    if (depth_ < Capacity) {
      stack_[depth_] = address;
    }
    ++depth_;
  }

  void pop() {
    if (depth_ > 0) {
      --depth_;
    }
  }

  int depth() const {
    return depth_;
  }

  int visibleDepth() const {
    return depth_ <= Capacity ? depth_ : 0;
  }

  std::uint32_t frameFromTop(int index) const {
    const int visible = visibleDepth();
    return index >= 0 && index < visible ? stack_[visible - 1 - index] : 0;
  }

  std::uint32_t top() const {
    return frameFromTop(0);
  }

  std::uint32_t caller() const {
    return frameFromTop(1);
  }

  class Scope {
  public:
    Scope(GuestCallAttribution &owner, std::uint32_t address) : owner_(owner) {
      owner_.push(address);
    }
    ~Scope() {
      owner_.pop();
    }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

  private:
    GuestCallAttribution &owner_;
  };

  [[nodiscard]] Scope scope(std::uint32_t address) {
    return Scope(*this, address);
  }

private:
  std::array<std::uint32_t, Capacity> stack_{};
  int depth_ = 0;
};

} // namespace psx::cpu

// code_memory.h — the recompiler's executable memory: one bounded mapping per executor, owned for its lifetime.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace psx::cpu::recompiler {

// One mapping of `capacity` bytes that holds translated host code. Blocks are bump-allocated and never freed one by
// one: when the mapping is full the owner drops every block and starts over (`reset`). On macOS the mapping is
// MAP_JIT and each thread writes only inside a `WriteScope` (pthread_jit_write_protect_np); elsewhere it is mapped
// read-write-execute. Nothing translated outlives the process or is written anywhere else.
class CodeMemory {
public:
  // Null when the host refuses executable memory; the executor then interprets.
  static std::unique_ptr<CodeMemory> create(std::size_t capacity);
  ~CodeMemory();
  CodeMemory(const CodeMemory &) = delete;
  CodeMemory &operator=(const CodeMemory &) = delete;

  // Makes the mapping writable (and not executable) on this thread for the scope's lifetime.
  class WriteScope {
  public:
    explicit WriteScope(CodeMemory &memory);
    ~WriteScope();
    WriteScope(const WriteScope &) = delete;
    WriteScope &operator=(const WriteScope &) = delete;
  };

  // The unwritten tail of the mapping, where the next block is assembled.
  [[nodiscard]] std::uint8_t *cursor() const {
    return base_ + used_;
  }
  [[nodiscard]] std::size_t remaining() const {
    return capacity_ - used_;
  }
  // Seals `bytes` written at `cursor()` as one block: aligned, its instruction cache synchronized.
  void commit(std::size_t bytes);
  // Forgets every block; the caller has dropped every pointer into the mapping.
  void reset();

  [[nodiscard]] std::size_t used() const {
    return used_;
  }
  [[nodiscard]] std::size_t capacity() const {
    return capacity_;
  }

private:
  CodeMemory(std::uint8_t *base, std::size_t capacity) : base_(base), capacity_(capacity) {}

  std::uint8_t *base_ = nullptr;
  std::size_t capacity_ = 0;
  std::size_t used_ = 0;
};

} // namespace psx::cpu::recompiler

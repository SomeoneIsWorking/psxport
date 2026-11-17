#include "code_memory.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>
#endif

namespace psx::cpu::recompiler {
namespace {

constexpr std::size_t kBlockAlignment = 16;

} // namespace

std::unique_ptr<CodeMemory> CodeMemory::create(std::size_t capacity) {
#if defined(_WIN32)
  void *memory = VirtualAlloc(nullptr, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if (memory == nullptr) {
    return nullptr;
  }
#else
  int flags = MAP_PRIVATE | MAP_ANON;
#if defined(__APPLE__)
  flags |= MAP_JIT;
#endif
  void *memory = mmap(nullptr, capacity, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
  if (memory == MAP_FAILED) {
    return nullptr;
  }
#endif
  return std::unique_ptr<CodeMemory>(new CodeMemory(static_cast<std::uint8_t *>(memory), capacity));
}

CodeMemory::~CodeMemory() {
#if defined(_WIN32)
  VirtualFree(base_, 0, MEM_RELEASE);
#else
  munmap(base_, capacity_);
#endif
}

CodeMemory::WriteScope::WriteScope(CodeMemory &) {
#if defined(__APPLE__) && defined(__aarch64__)
  pthread_jit_write_protect_np(0);
#endif
}

CodeMemory::WriteScope::~WriteScope() {
#if defined(__APPLE__) && defined(__aarch64__)
  pthread_jit_write_protect_np(1);
#endif
}

void CodeMemory::commit(std::size_t bytes) {
  std::uint8_t *const begin = base_ + used_;
  __builtin___clear_cache(reinterpret_cast<char *>(begin), reinterpret_cast<char *>(begin + bytes));
  used_ += (bytes + kBlockAlignment - 1) & ~(kBlockAlignment - 1);
  if (used_ > capacity_) {
    used_ = capacity_;
  }
}

void CodeMemory::reset() {
  used_ = 0;
}

} // namespace psx::cpu::recompiler

#include "catch2/catch.hpp"

#include <cstdint>
#include <cstdlib>
#include <sys/mman.h>

#include "rlbox_process_sandbox.hpp"

using rlbox::rlbox_process_sandbox;

/*
 * These tests exercise the pointer-translation, bounds-checking, and
 * sandbox-membership helpers on rlbox_process_sandbox WITHOUT spinning up
 * an actual sandboxed child.  We do that by deriving a thin harness that
 * lets us seed the shared-memory base/size directly.
 */
namespace {
class PointerTestHarness : public rlbox_process_sandbox
{
public:
  void set_shared_region(uintptr_t base, size_t size)
  {
    this->shared_memory_local_base = base;
    this->shared_memory_size = size;
  }

  using rlbox_process_sandbox::impl_get_memory_location;
  using rlbox_process_sandbox::impl_get_sandboxed_pointer;
  using rlbox_process_sandbox::impl_get_total_memory;
  using rlbox_process_sandbox::impl_get_unsandboxed_pointer;
  using rlbox_process_sandbox::impl_is_in_same_sandbox;
  using rlbox_process_sandbox::impl_is_pointer_in_app_memory;
  using rlbox_process_sandbox::impl_is_pointer_in_sandbox_memory;
};

constexpr uintptr_t kFakeBase = 0x400000000ull; // 16GB — safely aligned
constexpr size_t kFakeSize = 64ull * 1024ull * 1024ull;
} // namespace

TEST_CASE("pointer translation is reversible", "[pointers]")
{
  PointerTestHarness s;
  s.set_shared_region(kFakeBase, kFakeSize);

  // Offset 0 is semantically reserved to mean "null" in both directions
  // (see the early-return in impl_get_*andboxed_pointer), so the very
  // first byte of shared memory is unaddressable by design.  All other
  // in-range offsets must round-trip exactly.
  const uintptr_t offsets[] = { 0x1,      0x1000,        0x10000,
                                0x100000, kFakeSize - 1 };

  for (uintptr_t off : offsets) {
    void* unsandboxed = reinterpret_cast<void*>(kFakeBase + off);
    auto roundtrip = s.impl_get_sandboxed_pointer<char>(unsandboxed);
    CHECK(roundtrip == off);

    auto back = s.impl_get_unsandboxed_pointer<char>(off);
    CHECK(back == unsandboxed);
  }
}

TEST_CASE("nullptr/zero pointer translation is a no-op", "[pointers]")
{
  PointerTestHarness s;
  s.set_shared_region(kFakeBase, kFakeSize);

  CHECK(s.impl_get_unsandboxed_pointer<char>(0) == nullptr);
  CHECK(s.impl_get_sandboxed_pointer<char>(nullptr) == 0);
}

TEST_CASE("is_pointer_in_sandbox_memory respects bounds", "[pointers]")
{
  PointerTestHarness s;
  s.set_shared_region(kFakeBase, kFakeSize);

  // Inside
  CHECK(s.impl_is_pointer_in_sandbox_memory(
    reinterpret_cast<void*>(kFakeBase)));
  CHECK(s.impl_is_pointer_in_sandbox_memory(
    reinterpret_cast<void*>(kFakeBase + kFakeSize / 2)));
  CHECK(s.impl_is_pointer_in_sandbox_memory(
    reinterpret_cast<void*>(kFakeBase + kFakeSize - 1)));

  // Just outside (one byte past end — non-inclusive upper bound)
  CHECK_FALSE(s.impl_is_pointer_in_sandbox_memory(
    reinterpret_cast<void*>(kFakeBase + kFakeSize)));
  // Well below base
  CHECK_FALSE(s.impl_is_pointer_in_sandbox_memory(
    reinterpret_cast<void*>(kFakeBase - 1)));
  CHECK_FALSE(s.impl_is_pointer_in_sandbox_memory(
    reinterpret_cast<void*>(0x1000)));
}

TEST_CASE("is_pointer_in_app_memory is the complement of sandbox membership",
          "[pointers]")
{
  PointerTestHarness s;
  s.set_shared_region(kFakeBase, kFakeSize);

  int local_stack_var = 0;
  CHECK(s.impl_is_pointer_in_app_memory(&local_stack_var));
  CHECK_FALSE(s.impl_is_pointer_in_app_memory(
    reinterpret_cast<void*>(kFakeBase + 0x1000)));
}

TEST_CASE("is_in_same_sandbox returns true only when both pointers are inside",
          "[pointers]")
{
  PointerTestHarness s;
  s.set_shared_region(kFakeBase, kFakeSize);

  void* inside_a = reinterpret_cast<void*>(kFakeBase + 0x100);
  void* inside_b = reinterpret_cast<void*>(kFakeBase + 0x2000);
  int outside;

  CHECK(s.impl_is_in_same_sandbox(inside_a, inside_b));
  CHECK_FALSE(s.impl_is_in_same_sandbox(inside_a, &outside));
  CHECK_FALSE(s.impl_is_in_same_sandbox(&outside, inside_b));
}

TEST_CASE("get_total_memory and get_memory_location expose seeded state",
          "[pointers]")
{
  PointerTestHarness s;
  s.set_shared_region(kFakeBase, kFakeSize);
  CHECK(s.impl_get_total_memory() == kFakeSize);
  CHECK(reinterpret_cast<uintptr_t>(s.impl_get_memory_location()) ==
        kFakeBase);
}

TEST_CASE(
  "translation with zero base degrades gracefully (pre-create_sandbox state)",
  "[pointers]")
{
  PointerTestHarness s; // never initialized
  // With base = 0 the impl returns the input unchanged rather than
  // dereferencing garbage.  Documented behaviour — pin it in a test.
  void* raw = reinterpret_cast<void*>(0xDEADBEEFull);
  CHECK(s.impl_get_unsandboxed_pointer<char>(0xDEADBEEFull) == raw);
  CHECK(s.impl_get_sandboxed_pointer<char>(raw) ==
        static_cast<uintptr_t>(0xDEADBEEFull));
  CHECK_FALSE(s.impl_is_pointer_in_sandbox_memory(raw));
}

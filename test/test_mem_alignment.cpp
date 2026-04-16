#include "catch2/catch.hpp"

#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

#include "rlbox_process_mem.hpp"

using rlbox::os_mmap_aligned;

namespace {
constexpr size_t kOneMB = 1024ull * 1024ull;
constexpr size_t kFourGB = 0x100000000ull;

bool is_aligned(void* p, size_t alignment)
{
  return (reinterpret_cast<uintptr_t>(p) & (alignment - 1)) == 0;
}
} // namespace

TEST_CASE("os_mmap_aligned returns 4KB-aligned memory by default",
          "[mem][alignment]")
{
  long page = sysconf(_SC_PAGESIZE);
  REQUIRE(page > 0);

  void* p = os_mmap_aligned(kOneMB, static_cast<size_t>(page));
  REQUIRE(p != nullptr);
  REQUIRE(is_aligned(p, static_cast<size_t>(page)));

  munmap(p, kOneMB);
}

TEST_CASE("os_mmap_aligned produces 2MB alignment", "[mem][alignment]")
{
  constexpr size_t kTwoMB = 2ull * kOneMB;
  void* p = os_mmap_aligned(kOneMB, kTwoMB);
  REQUIRE(p != nullptr);
  REQUIRE(is_aligned(p, kTwoMB));
  munmap(p, kOneMB);
}

TEST_CASE("os_mmap_aligned produces 4GB alignment (matches sandbox config)",
          "[mem][alignment]")
{
  // This is the alignment actually used in impl_create_sandbox so it's the
  // most important invariant to validate.  If this ever fails, pointer
  // translation through the shared-memory base address will misbehave.
  void* p = os_mmap_aligned(kOneMB, kFourGB);
  REQUIRE(p != nullptr);
  REQUIRE(is_aligned(p, kFourGB));
  munmap(p, kOneMB);
}

TEST_CASE("os_mmap_aligned returns readable/writable memory when re-mapped",
          "[mem][alignment]")
{
  // The reservation from os_mmap_aligned is PROT_NONE. Overlay it with a
  // real mapping (as impl_create_sandbox does with the memfd) and make sure
  // the aligned address is usable.
  void* p = os_mmap_aligned(kOneMB, kOneMB);
  REQUIRE(p != nullptr);

  void* rw = mmap(p,
                  kOneMB,
                  PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                  -1,
                  0);
  REQUIRE(rw == p);

  std::memset(rw, 0xAB, kOneMB);
  auto* bytes = static_cast<unsigned char*>(rw);
  CHECK(bytes[0] == 0xAB);
  CHECK(bytes[kOneMB - 1] == 0xAB);

  munmap(rw, kOneMB);
}

TEST_CASE("os_mmap_aligned supports multiple concurrent reservations",
          "[mem][alignment]")
{
  void* a = os_mmap_aligned(kOneMB, kOneMB);
  void* b = os_mmap_aligned(kOneMB, kOneMB);
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  REQUIRE(a != b);
  REQUIRE(is_aligned(a, kOneMB));
  REQUIRE(is_aligned(b, kOneMB));

  munmap(a, kOneMB);
  munmap(b, kOneMB);
}

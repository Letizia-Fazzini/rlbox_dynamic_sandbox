#include "catch2/catch.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

#include "rlbox_process_abi.hpp"
#include "rlbox_process_sandbox.hpp"
#include "rlbox_process_tls.hpp"

#include "glue_lib.h"

using rlbox::rlbox_process_sandbox;

/*
 * Integration tests that spin up a real sandboxed child process.
 *
 * These depend on:
 *   - TEST_SANDBOX_WRAPPER_PATH : absolute path to the test wrapper
 *     executable that LD_PRELOADs sandbox_shim.so.  The wrapper statically
 *     links the test glue library so that dlsym(RTLD_DEFAULT, ...) can
 *     resolve glue_* symbols inside the child.
 *   - sandbox_shim.so must be present in the CWD when the test runs
 *     (impl_create_sandbox hard-codes LD_PRELOAD="./sandbox_shim.so").
 *
 * Ports are picked dynamically by the host via bind-to-0, so multiple
 * sandboxes can coexist and there's no TIME_WAIT cooldown between tests.
 */

#ifndef TEST_SANDBOX_WRAPPER_PATH
#  error \
    "TEST_SANDBOX_WRAPPER_PATH must be defined (see CMakeLists.txt)"
#endif

namespace {
// Exposes a few internals for targeted integration testing.
class IntegrationHarness : public rlbox_process_sandbox
{
public:
  rpc::client* raw_client() { return this->sandbox_client.get(); }
  using rlbox_process_sandbox::impl_free_in_sandbox;
  using rlbox_process_sandbox::impl_get_memory_location;
  using rlbox_process_sandbox::impl_get_sandboxed_pointer;
  using rlbox_process_sandbox::impl_get_total_memory;
  using rlbox_process_sandbox::impl_get_unsandboxed_pointer;
  using rlbox_process_sandbox::impl_invoke_with_func_ptr;
  using rlbox_process_sandbox::impl_is_pointer_in_app_memory;
  using rlbox_process_sandbox::impl_is_pointer_in_sandbox_memory;
  using rlbox_process_sandbox::impl_malloc_in_sandbox;
};
} // namespace

TEST_CASE("sandbox lifecycle: create then destroy cleanly",
          "[sandbox][lifecycle]")
{
  IntegrationHarness s;
  REQUIRE(s.impl_create_sandbox(TEST_SANDBOX_WRAPPER_PATH));

  // After create, we should have a shared-memory base and size.
  CHECK(s.impl_get_memory_location() != nullptr);
  CHECK(s.impl_get_total_memory() == 64ull * 1024ull * 1024ull);

  // TLS context should point at this sandbox.
  CHECK(rlbox::detail::thread_local_sandbox == &s);

  // RPC client should be live.
  CHECK(s.raw_client() != nullptr);

  s.impl_destroy_sandbox();

  // TLS pointer is cleared on destroy.
  CHECK(rlbox::detail::thread_local_sandbox == nullptr);
}

TEST_CASE("sandbox malloc returns an address inside shared memory",
          "[sandbox][malloc]")
{
  IntegrationHarness s;
  REQUIRE(s.impl_create_sandbox(TEST_SANDBOX_WRAPPER_PATH));

  auto addr = s.impl_malloc_in_sandbox(128);
  REQUIRE(addr != 0);

  // With same-base mapping the returned sandbox pointer is an absolute
  // address that also works on the host.  It must fall inside the mapped
  // region, and host writes through it must be immediately visible.
  void* host_ptr = s.impl_get_unsandboxed_pointer<char>(addr);
  CHECK(s.impl_is_pointer_in_sandbox_memory(host_ptr));

  std::memset(host_ptr, 0xCD, 128);
  CHECK(static_cast<unsigned char*>(host_ptr)[0] == 0xCD);
  CHECK(static_cast<unsigned char*>(host_ptr)[127] == 0xCD);

  s.impl_free_in_sandbox(addr);
  s.impl_destroy_sandbox();
}

TEST_CASE("sandbox malloc hands out distinct, non-overlapping regions",
          "[sandbox][malloc]")
{
  IntegrationHarness s;
  REQUIRE(s.impl_create_sandbox(TEST_SANDBOX_WRAPPER_PATH));

  constexpr size_t kN = 8;
  constexpr size_t kSize = 4096;
  std::vector<uintptr_t> addrs;
  addrs.reserve(kN);
  const auto base =
    reinterpret_cast<uintptr_t>(s.impl_get_memory_location());
  const auto total = s.impl_get_total_memory();
  for (size_t i = 0; i < kN; ++i) {
    auto addr = s.impl_malloc_in_sandbox(kSize);
    REQUIRE(addr != 0);
    addrs.push_back(addr);
  }

  // Every allocation must fit entirely within the shared region and must
  // not overlap any other.
  for (size_t i = 0; i < kN; ++i) {
    CHECK(addrs[i] >= base);
    CHECK(addrs[i] + kSize <= base + total);
    for (size_t j = i + 1; j < kN; ++j) {
      bool overlap =
        !(addrs[i] + kSize <= addrs[j] || addrs[j] + kSize <= addrs[i]);
      CHECK_FALSE(overlap);
    }
  }

  for (auto addr : addrs) {
    s.impl_free_in_sandbox(addr);
  }
  s.impl_destroy_sandbox();
}

TEST_CASE("sandbox malloc failure path: oversized allocation returns 0",
          "[sandbox][malloc]")
{
  IntegrationHarness s;
  REQUIRE(s.impl_create_sandbox(TEST_SANDBOX_WRAPPER_PATH));

  // Ask for more than the entire shared region — dlmalloc should refuse.
  auto off = s.impl_malloc_in_sandbox(s.impl_get_total_memory() * 2);
  CHECK(off == 0);

  s.impl_destroy_sandbox();
}

TEST_CASE("lookup_symbol resolves functions linked into the sandbox child",
          "[sandbox][symbol]")
{
  IntegrationHarness s;
  REQUIRE(s.impl_create_sandbox(TEST_SANDBOX_WRAPPER_PATH));

  // Call the shim's lookup_symbol handler directly.  Non-zero return means
  // dlsym resolved the symbol inside the child process.
  auto res = s.raw_client()->call("lookup_symbol", std::string("glue_add"));
  auto encoded_ptr = res.as<uintptr_t>();
  CHECK(encoded_ptr != 0);

  auto missing =
    s.raw_client()->call("lookup_symbol", std::string("not_a_real_symbol_xyz"));
  CHECK(missing.as<uintptr_t>() == 0);

  s.impl_destroy_sandbox();
}

TEST_CASE("invoke executes a simple C function via libffi in the child",
          "[sandbox][invoke]")
{
  IntegrationHarness s;
  REQUIRE(s.impl_create_sandbox(TEST_SANDBOX_WRAPPER_PATH));

  // Resolve glue_add in the child.
  auto lookup =
    s.raw_client()->call("lookup_symbol", std::string("glue_add"));
  auto encoded = lookup.as<uintptr_t>();
  REQUIRE(encoded != 0);

  // New typed schema: (func_offset, ret_tag, arg_tags, arg_values).
  std::vector<int32_t> arg_tags = { rlbox::ARG_SINT64, rlbox::ARG_SINT64 };
  std::vector<int64_t> arg_values = { 7, 35 };
  auto result = s.raw_client()->call(
    "invoke", encoded, (int32_t)rlbox::ARG_SINT64, arg_tags, arg_values);
  CHECK(result.as<int64_t>() == 42);

  s.impl_destroy_sandbox();
}

TEST_CASE("invoke writes through a POINTER-tagged arg into shared memory",
          "[sandbox][invoke][memory]")
{
  IntegrationHarness s;
  REQUIRE(s.impl_create_sandbox(TEST_SANDBOX_WRAPPER_PATH));

  // Allocate an int64 inside the sandbox and initialize to 0.
  auto addr = s.impl_malloc_in_sandbox(sizeof(int64_t));
  REQUIRE(addr != 0);
  auto* host_view = static_cast<int64_t*>(
    s.impl_get_unsandboxed_pointer<int64_t>(addr));
  *host_view = 0;

  // Resolve glue_write_int64: int64_t(int64_t*, int64_t).
  auto lookup =
    s.raw_client()->call("lookup_symbol", std::string("glue_write_int64"));
  auto encoded = lookup.as<uintptr_t>();
  REQUIRE(encoded != 0);

  // Send the pointer arg as an ARG_POINTER carrying an absolute address
  // (same-base mapping makes host and child addresses identical for the
  // shared region).  The callee writes through the pointer and the host
  // sees the write immediately via MAP_SHARED.
  std::vector<int32_t> arg_tags = { rlbox::ARG_POINTER, rlbox::ARG_SINT64 };
  std::vector<int64_t> arg_values = { static_cast<int64_t>(addr),
                                      0xDEADBEEFLL };
  auto result = s.raw_client()->call(
    "invoke", encoded, (int32_t)rlbox::ARG_SINT64, arg_tags, arg_values);

  CHECK(result.as<int64_t>() == 0xDEADBEEFLL);
  CHECK(*host_view == 0xDEADBEEFLL);

  s.impl_free_in_sandbox(addr);
  s.impl_destroy_sandbox();
}

TEST_CASE("impl_invoke_with_func_ptr emits typed payload and round-trips",
          "[sandbox][invoke][typed]")
{
  // Exercise the high-level wrapper that derives the typed payload from
  // the function type T.  This is the path rlbox itself uses.
  IntegrationHarness s;
  REQUIRE(s.impl_create_sandbox(TEST_SANDBOX_WRAPPER_PATH));

  auto lookup =
    s.raw_client()->call("lookup_symbol", std::string("glue_add"));
  auto encoded = lookup.as<uintptr_t>();
  REQUIRE(encoded != 0);

  using Func_T = int64_t(int64_t, int64_t);
  auto* func_ptr = reinterpret_cast<Func_T*>(encoded);

  // T is the original function type.  T_Converted is deduced from the
  // func_ptr argument; T_Args are deduced from the call site.
  int64_t out = s.template impl_invoke_with_func_ptr<Func_T>(
    func_ptr, int64_t{ 100 }, int64_t{ 23 });
  CHECK(out == 123);

  s.impl_destroy_sandbox();
}

TEST_CASE("callback registration uses a trampoline slot and can unregister",
          "[sandbox][callback]")
{
  IntegrationHarness s;
  REQUIRE(s.impl_create_sandbox(TEST_SANDBOX_WRAPPER_PATH));

  // Pick an arbitrary host-side key.
  void* key = reinterpret_cast<void*>(0xCAFEull);
  auto tramp = s.impl_register_callback<int64_t, int64_t>(key, key);
  CHECK(tramp != 0);

  s.impl_unregister_callback<int64_t, int64_t>(key);

  // After unregister, a fresh registration should succeed again (slot
  // returned to the pool).
  auto tramp2 = s.impl_register_callback<int64_t, int64_t>(key, key);
  CHECK(tramp2 != 0);
  s.impl_unregister_callback<int64_t, int64_t>(key);

  s.impl_destroy_sandbox();
}

TEST_CASE("callback pool is bounded at 16 slots",
          "[sandbox][callback][limits]")
{
  IntegrationHarness s;
  REQUIRE(s.impl_create_sandbox(TEST_SANDBOX_WRAPPER_PATH));

  std::vector<void*> keys;
  for (int i = 0; i < 16; ++i) {
    auto* k = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000 + i));
    auto tramp = s.impl_register_callback<int64_t, int64_t>(k, k);
    CHECK(tramp != 0);
    keys.push_back(k);
  }

  // 17th registration must fail (return 0) — pool is full.
  auto* overflow_key = reinterpret_cast<void*>(0x2000ull);
  auto overflow = s.impl_register_callback<int64_t, int64_t>(overflow_key,
                                                             overflow_key);
  CHECK(overflow == 0);

  for (auto* k : keys) {
    s.impl_unregister_callback<int64_t, int64_t>(k);
  }

  s.impl_destroy_sandbox();
}

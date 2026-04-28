#include "catch2/catch.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

#include "rlbox_process_abi.hpp"
#include "rlbox_process_sandbox.hpp"
#include "rlbox_process_tls.hpp"

#include "glue_lib.h"

using rlbox::rlbox_process_sandbox;

// Integration tests that spin up a real sandboxed child process.
// Requires TEST_SANDBOX_WRAPPER_PATH (set by CMake) and ./sandbox_shim.so
// in CWD (impl_create_sandbox hard-codes LD_PRELOAD).

#ifndef TEST_SANDBOX_WRAPPER_PATH
#  error \
    "TEST_SANDBOX_WRAPPER_PATH must be defined (see CMakeLists.txt)"
#endif

namespace {
// Transport-agnostic harness exposing select internals for testing.
class IntegrationHarness : public rlbox_process_sandbox
{
public:
  bool transport_alive() const
  {
#if defined(RLBOX_TRANSPORT_RPCLIB)
    return this->sandbox_client != nullptr;
#elif defined(RLBOX_TRANSPORT_CAPNP)
    return this->request_fd >= 0;
#else
    return false;
#endif
  }

  uintptr_t raw_lookup_symbol(const std::string& name)
  {
    return reinterpret_cast<uintptr_t>(this->impl_lookup_symbol(name.c_str()));
  }

  // Send the wire-schema invoke payload directly (no template plumbing).
  int64_t raw_invoke(uintptr_t func_addr,
                     int32_t ret_tag,
                     std::vector<int32_t> tags,
                     std::vector<int64_t> values)
  {
#if defined(RLBOX_TRANSPORT_RPCLIB)
    auto result = this->sandbox_client->call(
      "invoke", func_addr, ret_tag, tags, values);
    return result.template as<int64_t>();
#elif defined(RLBOX_TRANSPORT_CAPNP)
    return this->capnp_call([&](rlbox::wire::Request::Builder& req) {
      auto inv = req.initInvoke();
      inv.setFuncAddr(func_addr);
      inv.setRetTag(ret_tag);
      auto t = inv.initArgTags(tags.size());
      for (size_t i = 0; i < tags.size(); ++i) {
        t.set(i, tags[i]);
      }
      auto v = inv.initArgValues(values.size());
      for (size_t i = 0; i < values.size(); ++i) {
        v.set(i, values[i]);
      }
    });
#else
    return 0;
#endif
  }

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

  CHECK(s.impl_get_memory_location() != nullptr);
  CHECK(s.impl_get_total_memory() == RLBOX_SHM_REGION_BYTES);
  CHECK(rlbox::detail::thread_local_sandbox == &s);
  CHECK(s.transport_alive());

  s.impl_destroy_sandbox();

  CHECK(rlbox::detail::thread_local_sandbox == nullptr);
}

TEST_CASE("sandbox malloc returns an address inside shared memory",
          "[sandbox][malloc]")
{
  IntegrationHarness s;
  REQUIRE(s.impl_create_sandbox(TEST_SANDBOX_WRAPPER_PATH));

  auto addr = s.impl_malloc_in_sandbox(128);
  REQUIRE(addr != 0);

  // Same-base mapping: addr is a host-valid absolute address inside the
  // mapped region; writes through it are visible to the child via MAP_SHARED.
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

  // Each allocation must fit in the shared region and not overlap any other.
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

  // Ask for more than the shared region; dlmalloc should refuse.
  auto off = s.impl_malloc_in_sandbox(s.impl_get_total_memory() * 2);
  CHECK(off == 0);

  s.impl_destroy_sandbox();
}

TEST_CASE("lookup_symbol resolves functions linked into the sandbox child",
          "[sandbox][symbol]")
{
  IntegrationHarness s;
  REQUIRE(s.impl_create_sandbox(TEST_SANDBOX_WRAPPER_PATH));

  // Call the shim's lookup_symbol handler directly. Non-zero means dlsym
  // resolved the symbol inside the child.
  auto encoded_ptr = s.raw_lookup_symbol("glue_add");
  CHECK(encoded_ptr != 0);

  auto missing = s.raw_lookup_symbol("not_a_real_symbol_xyz");
  CHECK(missing == 0);

  s.impl_destroy_sandbox();
}

TEST_CASE("invoke executes a simple C function via libffi in the child",
          "[sandbox][invoke]")
{
  IntegrationHarness s;
  REQUIRE(s.impl_create_sandbox(TEST_SANDBOX_WRAPPER_PATH));

  auto encoded = s.raw_lookup_symbol("glue_add");
  REQUIRE(encoded != 0);

  std::vector<int32_t> arg_tags = { rlbox::ARG_SINT64, rlbox::ARG_SINT64 };
  std::vector<int64_t> arg_values = { 7, 35 };
  int64_t result =
    s.raw_invoke(encoded, (int32_t)rlbox::ARG_SINT64, arg_tags, arg_values);
  CHECK(result == 42);

  s.impl_destroy_sandbox();
}

TEST_CASE("invoke writes through a POINTER-tagged arg into shared memory",
          "[sandbox][invoke][memory]")
{
  IntegrationHarness s;
  REQUIRE(s.impl_create_sandbox(TEST_SANDBOX_WRAPPER_PATH));

  auto addr = s.impl_malloc_in_sandbox(sizeof(int64_t));
  REQUIRE(addr != 0);
  auto* host_view = static_cast<int64_t*>(
    s.impl_get_unsandboxed_pointer<int64_t>(addr));
  *host_view = 0;

  auto encoded = s.raw_lookup_symbol("glue_write_int64");
  REQUIRE(encoded != 0);

  // ARG_POINTER carries a 32-bit shared-region offset on the wire (Phase
  // 7b); shim adds its base back before passing to the library, then writes
  // through it.  Host sees the result via MAP_SHARED.
  auto base =
    reinterpret_cast<uintptr_t>(s.impl_get_memory_location());
  int64_t addr_off = static_cast<int64_t>(static_cast<uintptr_t>(addr) - base);
  std::vector<int32_t> arg_tags = { rlbox::ARG_POINTER, rlbox::ARG_SINT64 };
  std::vector<int64_t> arg_values = { addr_off, 0xDEADBEEFLL };
  int64_t result =
    s.raw_invoke(encoded, (int32_t)rlbox::ARG_SINT64, arg_tags, arg_values);

  CHECK(result == 0xDEADBEEFLL);
  CHECK(*host_view == 0xDEADBEEFLL);

  s.impl_free_in_sandbox(addr);
  s.impl_destroy_sandbox();
}

TEST_CASE("impl_invoke_with_func_ptr emits typed payload and round-trips",
          "[sandbox][invoke][typed]")
{
  // Exercise the high-level wrapper that derives the typed payload from T.
  IntegrationHarness s;
  REQUIRE(s.impl_create_sandbox(TEST_SANDBOX_WRAPPER_PATH));

  auto encoded = s.raw_lookup_symbol("glue_add");
  REQUIRE(encoded != 0);

  using Func_T = int64_t(int64_t, int64_t);
  auto* func_ptr = reinterpret_cast<Func_T*>(encoded);

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

  void* key = reinterpret_cast<void*>(0xCAFEull);
  auto tramp = s.impl_register_callback<int64_t, int64_t>(key, key);
  CHECK(tramp != 0);

  s.impl_unregister_callback<int64_t, int64_t>(key);

  // After unregister the slot returns to the pool, so re-registration works.
  auto tramp2 = s.impl_register_callback<int64_t, int64_t>(key, key);
  CHECK(tramp2 != 0);
  s.impl_unregister_callback<int64_t, int64_t>(key);

  s.impl_destroy_sandbox();
}

// Mirrors the shim's RLBOX_CALLBACK_SLOTS default. Kept in sync manually.
static constexpr int kCallbackSlotsForTest = 64;

TEST_CASE("callback pool fills exactly its slot count, then refuses more",
          "[sandbox][callback][limits]")
{
  IntegrationHarness s;
  REQUIRE(s.impl_create_sandbox(TEST_SANDBOX_WRAPPER_PATH));

  std::vector<void*> keys;
  for (int i = 0; i < kCallbackSlotsForTest; ++i) {
    auto* k = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000 + i));
    auto tramp = s.impl_register_callback<int64_t, int64_t>(k, k);
    CHECK(tramp != 0);
    keys.push_back(k);
  }

  // One past the pool size must fail (pool full).
  auto* overflow_key = reinterpret_cast<void*>(0x2000ull);
  auto overflow = s.impl_register_callback<int64_t, int64_t>(overflow_key,
                                                             overflow_key);
  CHECK(overflow == 0);

  for (auto* k : keys) {
    s.impl_unregister_callback<int64_t, int64_t>(k);
  }

  s.impl_destroy_sandbox();
}

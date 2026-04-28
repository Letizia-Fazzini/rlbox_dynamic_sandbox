// Cross-backend pointer integration test for rlbox_meta_sandbox.
//
// Mints a buffer through one backend's allocator and checksums it via
// the other backend's invoke path -- proves a sandbox pointer survives
// cross-backend pass-through under shared memory.  Uses adler32 because
// it's available as an export on both wasm2c and the process-side libz.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define RLBOX_SINGLE_THREADED_INVOCATIONS

#define RLBOX_WASM2C_MODULE_NAME zlib
#include "zlib.wasm.h"
#include "rlbox.hpp"
#include "rlbox_meta_sandbox.hpp"
#include "rlbox_process_abi.hpp"
#include "zlib.h"
#include "zlib_structs.h"

using namespace rlbox;

rlbox_load_structs_from_library(zlib);
RLBOX_DEFINE_BASE_TYPES_FOR(zlib, meta);
rlbox_meta_load_struct_translators(zlib);

#define check(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: " msg "\n");                                 \
      std::abort();                                                            \
    }                                                                          \
  } while (0)

namespace {

constexpr size_t kBufBytes = 4096;

void fill_pattern(uint8_t* dst, size_t n)
{
  // Non-trivial pattern so adler32 gives a recognizable nonzero value and
  // any byte-shuffle across the boundary would show up.
  for (size_t i = 0; i < n; ++i) {
    dst[i] = static_cast<uint8_t>((i * 31u + 7u) & 0xFFu);
  }
}

uLong canonical_adler(const uint8_t* buf, size_t n)
{
  uLong a = adler32(0L, Z_NULL, 0);
  return adler32(a, buf, static_cast<uInt>(n));
}

uintptr_t shared_base(rlbox_meta_sandbox& meta)
{
  return reinterpret_cast<uintptr_t>(
    meta.get_process_sbx().impl_get_memory_location());
}

} // namespace

int main()
{
  std::printf("test_cross_pointer: starting\n");

  rlbox_sandbox_zlib sandbox;
  bool ok = sandbox.create_sandbox(ZLIB_PROCESS_WRAPPER_PATH);
  check(ok, "create_sandbox failed");

  auto* meta = sandbox.get_sandbox_impl();
  rlbox::meta_zlib_setup(*meta);
  // Disable symbol pinning so policy is consulted on every invoke (this
  // test deliberately switches policy mid-stream).
  meta->set_pin_threshold(0);

  uint8_t pattern[kBufBytes];
  fill_pattern(pattern, kBufBytes);
  uLong canonical = canonical_adler(pattern, kBufBytes);

  uintptr_t base = shared_base(*meta);

  // ---- Direction 1: wasm-allocated buffer, process-invoked checksum ----
  meta->set_policy(
    [](const meta_policy_context&) { return meta_backend::wasm; });

  auto wasm_buf = sandbox.malloc_in_sandbox<uint8_t>(kBufBytes);
  check(wasm_buf != nullptr, "wasm-route malloc returned null");

  uintptr_t wasm_va =
    reinterpret_cast<uintptr_t>(wasm_buf.UNSAFE_unverified());
  uintptr_t wasm_off = wasm_va - base;
  std::printf("  wasm-mint buf: host VA = %p, offset = 0x%lx\n",
              reinterpret_cast<void*>(wasm_va),
              static_cast<unsigned long>(wasm_off));
  check(wasm_off < RLBOX_SHM_PROCESS_OFFSET,
        "wasm-route malloc landed outside the wasm partition");

  // Write through the host VA -- both backends share the memfd, so any
  // dereference (host, process child, wasm side) sees this data.
  std::memcpy(reinterpret_cast<void*>(wasm_va), pattern, kBufBytes);

  meta->set_policy(
    [](const meta_policy_context&) { return meta_backend::process; });

  auto adler_ret_proc = sandbox.invoke_sandbox_function(
    adler32,
    static_cast<uLong>(1),
    wasm_buf,
    static_cast<uInt>(kBufBytes));
  uLong got_proc =
    adler_ret_proc.copy_and_verify([](uLong v) { return v; });
  std::printf("  process-route adler over wasm-mint buffer: 0x%08lx (want 0x%08lx)\n",
              static_cast<unsigned long>(got_proc),
              static_cast<unsigned long>(canonical));
  check(got_proc == canonical,
        "process-route adler over wasm-mint buffer mismatch");

  // Free the wasm-route buffer; partition routing in impl_free_in_sandbox
  // dispatches to the wasm allocator because the offset is in the low
  // partition.
  meta->set_policy(
    [](const meta_policy_context&) { return meta_backend::wasm; });
  sandbox.free_in_sandbox(wasm_buf);

  // ---- Direction 2: process-allocated buffer, wasm-invoked checksum ----
  meta->set_policy(
    [](const meta_policy_context&) { return meta_backend::process; });

  auto proc_buf = sandbox.malloc_in_sandbox<uint8_t>(kBufBytes);
  check(proc_buf != nullptr, "process-route malloc returned null");

  uintptr_t proc_va =
    reinterpret_cast<uintptr_t>(proc_buf.UNSAFE_unverified());
  uintptr_t proc_off = proc_va - base;
  std::printf("  process-mint buf: host VA = %p, offset = 0x%lx\n",
              reinterpret_cast<void*>(proc_va),
              static_cast<unsigned long>(proc_off));
  check(proc_off >= RLBOX_SHM_PROCESS_OFFSET,
        "process-route malloc landed outside the process partition");
  check(proc_off < RLBOX_SHM_REGION_BYTES,
        "process-route malloc landed outside the shared region");

  std::memcpy(reinterpret_cast<void*>(proc_va), pattern, kBufBytes);

  meta->set_policy(
    [](const meta_policy_context&) { return meta_backend::wasm; });

  auto adler_ret_wasm = sandbox.invoke_sandbox_function(
    adler32,
    static_cast<uLong>(1),
    proc_buf,
    static_cast<uInt>(kBufBytes));
  uLong got_wasm =
    adler_ret_wasm.copy_and_verify([](uLong v) { return v; });
  std::printf("  wasm-route adler over process-mint buffer: 0x%08lx (want 0x%08lx)\n",
              static_cast<unsigned long>(got_wasm),
              static_cast<unsigned long>(canonical));
  check(got_wasm == canonical,
        "wasm-route adler over process-mint buffer mismatch");

  meta->set_policy(
    [](const meta_policy_context&) { return meta_backend::process; });
  sandbox.free_in_sandbox(proc_buf);

  sandbox.destroy_sandbox();

  std::printf("PASS: cross-backend pointer pass-through both directions\n");
  return 0;
}

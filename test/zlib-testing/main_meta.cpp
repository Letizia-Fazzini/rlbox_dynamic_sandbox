// Meta-sandbox driver for zlib: composes process + wasm2c behind one rlbox
// handle and dispatches each invoke through a runtime-selectable policy.
// Same workload shape as main_process.cpp so the bench harness can compare
// directly against pure backends.
//
//   ./main_meta <level> <policy>
//     level  in 1..9 (default 2)
//     policy in {process, wasm, adaptive} (default process)
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>

#define CHUNK 16384

static double monotonic_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

#define release_assert(cond, msg) \
  if (!(cond)) {                  \
    fputs(msg "\n", stderr);      \
    abort();                      \
  }

#define RLBOX_SINGLE_THREADED_INVOCATIONS

// Wasm preamble for the meta header. Do NOT define RLBOX_USE_STATIC_CALLS:
// the meta resolves symbols dynamically per backend; the static-calls
// fast path would bind host VAs at compile time and silently no-op the
// process route.
#define RLBOX_WASM2C_MODULE_NAME zlib
#include "zlib.wasm.h"
#include "rlbox.hpp"
#include "rlbox_meta_sandbox.hpp"
#include "zlib.h"
#include "zlib_structs.h"

using namespace rlbox;

rlbox_load_structs_from_library(zlib);
RLBOX_DEFINE_BASE_TYPES_FOR(zlib, meta);

// Emits per-struct translators + meta_zlib_setup so wasm-side invokes
// crossing a registered struct get per-field ABI width adjustment.
// Must come after rlbox_load_structs_from_library.
rlbox_meta_load_struct_translators(zlib);

extern "C" {
int deflateInitWrapper(z_streamp strm, int level);
int inflateInitWrapper(z_streamp strm);
}

int main(int argc, char const* argv[]) {
  int level = 2;
  std::string policy_name = "process";
  if (argc > 1) level = std::stoi(argv[1]);
  if (argc > 2) policy_name = argv[2];

  rlbox_sandbox_zlib sandbox;
  sandbox.create_sandbox(ZLIB_PROCESS_WRAPPER_PATH);

  // Register zlib struct translators; without this a wasm-route invoke
  // crossing a registered struct pointer would corrupt fields against
  // the host ABI.
  rlbox::meta_zlib_setup(*sandbox.get_sandbox_impl());

  // Apply the requested dispatch policy. `process` matches the default,
  // installed explicitly so the binary's intent is visible.
  if (policy_name == "process") {
    sandbox.get_sandbox_impl()->set_policy(
        [](const meta_policy_context&) { return meta_backend::process; });
  } else if (policy_name == "wasm") {
    sandbox.get_sandbox_impl()->set_policy(
        [](const meta_policy_context&) { return meta_backend::wasm; });
  } else if (policy_name == "adaptive") {
    // explore=3 invokes per backend, alloc_warmup=1000: probes wasm long
    // enough to keep zlib's per-chunk allocations from pinning to process.
    sandbox.get_sandbox_impl()->set_policy(
        rlbox::make_adaptive_policy(3, 1000));
  } else {
    fprintf(stderr, "unknown policy '%s' (try process|wasm|adaptive)\n",
            policy_name.c_str());
    return 2;
  }

  FILE* source = fopen("pi.txt", "r");
  FILE* dest = fopen("compressed.txt", "w");
  release_assert(source && dest,
                 "could not open test_data.txt / compressed.txt");

  int flush;
  unsigned have;
  z_stream initStream;
  unsigned char in[CHUNK];

  initStream.zalloc = Z_NULL;
  initStream.zfree = Z_NULL;
  initStream.opaque = Z_NULL;

  auto sandboxedStream = sandbox.malloc_in_sandbox<z_stream>();
  rlbox::memcpy(sandbox, sandboxedStream, &initStream, sizeof(z_stream));

  auto deflateInitRet = sandbox.invoke_sandbox_function(deflateInitWrapper,
                                                        sandboxedStream, level);
  auto verifiedRet = deflateInitRet.copy_and_verify([](int val) {
    release_assert(val >= -6 && val <= 2, "Invalid ZLIB error code");
    return val;
  });
  if (verifiedRet != Z_OK) {
    return Z_ERRNO;
  }

  auto verifiedAvailOut = 0;
  auto verifiedDeflateRet = Z_OK;

  double t_sandbox_ms = 0.0, t0;

  do {
    auto in_size = fread(in, 1, CHUNK, source);
    flush = feof(source) ? Z_FINISH : Z_NO_FLUSH;

    t0 = monotonic_ms();
    sandboxedStream->avail_in = in_size;
    if (ferror(source)) {
      (void)sandbox.invoke_sandbox_function(deflateEnd, sandboxedStream);
      return Z_ERRNO;
    }

    // malloc_in_sandbox(0) aborts on wasm2c; pad on the empty Z_FINISH iter.
    auto sandboxedIn =
        sandbox.malloc_in_sandbox<char>(in_size > 0 ? in_size : 1);
    if (in_size > 0) {
      rlbox::memcpy(sandbox, sandboxedIn, &in, in_size);
    }
    sandboxedStream->next_in = sandboxedIn;
    t_sandbox_ms += monotonic_ms() - t0;

    do {
      t0 = monotonic_ms();
      auto sandboxedOut = sandbox.malloc_in_sandbox<char>(CHUNK);
      sandboxedStream->avail_out = CHUNK;
      sandboxedStream->next_out = sandboxedOut;

      auto deflateRet =
          sandbox.invoke_sandbox_function(deflate, sandboxedStream, flush);
      verifiedDeflateRet = deflateRet.copy_and_verify([](int val) {
        release_assert(val >= -6 && val <= 2, "Invalid ZLIB error code");
        return val;
      });

      assert(verifiedDeflateRet != Z_STREAM_ERROR);

      verifiedAvailOut = sandboxedStream->avail_out.copy_and_verify(
          [](int val) { return val; });

      have = CHUNK - verifiedAvailOut;
      if (have > 0) {
        std::unique_ptr<char[]> uniqueOut = sandboxedOut.copy_and_verify_range(
            [](std::unique_ptr<char[]> val) { return std::move(val); }, have);

        char* out = uniqueOut.release();
        if (fwrite(out, 1, have, dest) != have || ferror(dest)) {
          (void)sandbox.invoke_sandbox_function(deflateEnd, sandboxedStream);
          return Z_ERRNO;
        }
        sandbox.free_in_sandbox(sandboxedOut);
      }
      t_sandbox_ms += monotonic_ms() - t0;

    } while (verifiedAvailOut == 0);

    t0 = monotonic_ms();
    sandbox.free_in_sandbox(sandboxedIn);

    auto verifiedAvailIn =
        sandboxedStream->avail_in.copy_and_verify([](int val) {
          release_assert(val <= CHUNK, "Unread input cannot exceed CHUNK");
          return val;
        });
    assert(verifiedAvailIn == 0);
    t_sandbox_ms += monotonic_ms() - t0;

  } while (flush != Z_FINISH);

  assert(verifiedDeflateRet == Z_STREAM_END);

  fclose(source);
  fclose(dest);

  sandbox.destroy_sandbox();

  printf("COMPRESSION_MS=%.3f\n", t_sandbox_ms);
  return 0;
}

// One-shot zlib compression via rlbox_process_sandbox — counterpart to
// main_stress.cpp (wasm2c). Same shape: read the entire input, allocate one
// sandbox input + one compressBound()-sized output, and invoke
// deflate(Z_FINISH) exactly once. With a single invoke, the per-call fork()
// cost is paid once per stream instead of per 16KB chunk — which is where
// the process backend stops being hundreds-of-forks-bottlenecked and native
// execution (no wasm2c bounds checks, no 32-bit-heap layout) can actually
// win on compression-heavy streams.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <vector>

static double monotonic_ms()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

#define release_assert(cond, msg) if (!(cond)) { fputs(msg "\n", stderr); abort(); }

#define RLBOX_SINGLE_THREADED_INVOCATIONS

#include "rlbox.hpp"
#include "rlbox_process_sandbox.hpp"
#include "zlib.h"
#include "zlib_structs.h"

using namespace rlbox;

rlbox_load_structs_from_library(zlib);
RLBOX_DEFINE_BASE_TYPES_FOR(zlib, process);

extern "C" {
  int deflateInitWrapper(z_streamp strm, int level);
}

int main(int argc, char const *argv[]) {
  int level = 6;
  if (argc > 1) level = std::stoi(argv[1]);

  rlbox_sandbox_zlib sandbox;
  sandbox.create_sandbox(ZLIB_PROCESS_WRAPPER_PATH);

  FILE* source = fopen("pi.txt", "r");
  FILE* dest   = fopen("compressed.txt", "w");
  release_assert(source && dest, "could not open pi.txt / compressed.txt");

  fseek(source, 0, SEEK_END);
  size_t in_size = (size_t)ftell(source);
  fseek(source, 0, SEEK_SET);
  std::vector<unsigned char> in_buf(in_size);
  release_assert(fread(in_buf.data(), 1, in_size, source) == in_size,
                 "short read on pi.txt");
  fclose(source);

  z_stream initStream{};
  initStream.zalloc = Z_NULL;
  initStream.zfree  = Z_NULL;
  initStream.opaque = Z_NULL;

  auto sandboxedStream = sandbox.malloc_in_sandbox<z_stream>();
  rlbox::memcpy(sandbox, sandboxedStream, &initStream, sizeof(z_stream));

  auto initRet = sandbox.invoke_sandbox_function(
      deflateInitWrapper, sandboxedStream, level);
  auto verifiedInit = initRet.copy_and_verify([](int v){
    release_assert(v >= -6 && v <= 2, "Invalid ZLIB error code");
    return v;
  });
  if (verifiedInit != Z_OK) return Z_ERRNO;

  size_t out_cap = compressBound((uLong)in_size);

  double t0 = monotonic_ms();

  auto sandboxedIn  = sandbox.malloc_in_sandbox<char>(in_size > 0 ? in_size : 1);
  auto sandboxedOut = sandbox.malloc_in_sandbox<char>(out_cap);
  if (in_size > 0) {
    rlbox::memcpy(sandbox, sandboxedIn, in_buf.data(), in_size);
  }

  sandboxedStream->next_in   = sandboxedIn;
  sandboxedStream->avail_in  = (uInt)in_size;
  sandboxedStream->next_out  = sandboxedOut;
  sandboxedStream->avail_out = (uInt)out_cap;

  auto deflateRet = sandbox.invoke_sandbox_function(
      deflate, sandboxedStream, Z_FINISH);
  auto verifiedDeflate = deflateRet.copy_and_verify([](int v){
    release_assert(v >= -6 && v <= 2, "Invalid ZLIB error code");
    return v;
  });
  assert(verifiedDeflate == Z_STREAM_END);

  auto verifiedAvailOut = sandboxedStream->avail_out.copy_and_verify(
      [out_cap](uInt v){
        release_assert(v <= out_cap, "avail_out larger than allocation");
        return v;
      });
  size_t have = out_cap - verifiedAvailOut;

  auto uniqueOut = sandboxedOut.copy_and_verify_range(
      [](std::unique_ptr<char[]> val){ return std::move(val); }, have);

  double t_sandbox_ms = monotonic_ms() - t0;

  release_assert(fwrite(uniqueOut.get(), 1, have, dest) == have,
                 "short write on compressed.txt");
  fclose(dest);

  (void)sandbox.invoke_sandbox_function(deflateEnd, sandboxedStream);
  sandbox.free_in_sandbox(sandboxedIn);
  sandbox.free_in_sandbox(sandboxedOut);
  sandbox.destroy_sandbox();

  printf("COMPRESSION_MS=%.3f\n", t_sandbox_ms);
  return 0;
}

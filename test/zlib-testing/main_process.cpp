#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#define CHUNK 16384

static double monotonic_ms()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

#define release_assert(cond, msg) if (!(cond)) { fputs(msg "\n", stderr); abort(); }

// We're going to use RLBox in a single-threaded environment.
#define RLBOX_SINGLE_THREADED_INVOCATIONS

// Process sandbox uses dynamic symbol resolution via dlsym in the child —
// no RLBOX_USE_STATIC_CALLS() define.

#include "rlbox.hpp"
#include "rlbox_process_sandbox.hpp"
#include "zlib.h"
#include "zlib_structs.h"

using namespace rlbox;

rlbox_load_structs_from_library(zlib);

RLBOX_DEFINE_BASE_TYPES_FOR(zlib, process);

// Wrapper functions live in zlib_wrapper.c inside the child; the child
// exposes them via -rdynamic so dlsym(RTLD_DEFAULT, ...) finds them.
extern "C" {
  int deflateInitWrapper(z_streamp strm, int level);
  int inflateInitWrapper(z_streamp strm);
}

int main(int argc, char const *argv[]) {

  rlbox_sandbox_zlib sandbox;
  sandbox.create_sandbox(ZLIB_PROCESS_WRAPPER_PATH);

  FILE* source = fopen("pi.txt","r");
  FILE* dest = fopen("compressed.txt", "w");
  int  flush;
  unsigned have;
  z_stream initStream;
  unsigned char in[CHUNK];

  FILE* Bsource = fopen("pi.txt","r");
  FILE* Bdest = fopen("compressed_baseline.txt", "w");
  int  Bflush, Bret;
  unsigned Bhave;
  z_stream Bstrm;
  unsigned char Bin[CHUNK];
  unsigned char Bout[CHUNK];

  int level = 2;
  if(argc>1) {
    level = std::stoi(argv[1]);
  }

  initStream.zalloc = Z_NULL;
  initStream.zfree = Z_NULL;
  initStream.opaque = Z_NULL;

  Bstrm.zalloc = Z_NULL;
  Bstrm.zfree = Z_NULL;
  Bstrm.opaque = Z_NULL;

  auto sandboxedStream = sandbox.malloc_in_sandbox<z_stream>();
  rlbox::memcpy(sandbox, sandboxedStream, &initStream, sizeof(z_stream));

  auto deflateInitRet = sandbox.invoke_sandbox_function(
    deflateInitWrapper, sandboxedStream, level);

  auto verifiedRet = deflateInitRet.copy_and_verify([](int val){
    release_assert(val >= -6 && val <= 2, "Invalid ZLIB error code");
    return val;
  });
  if (verifiedRet != Z_OK){
    return Z_ERRNO;
  }

  Bret = deflateInit(&Bstrm, level);
  if (Bret != Z_OK){
    return Z_ERRNO;
  }

  auto verifiedAvailOut = 0;
  auto verifiedDeflateRet = Z_OK;

  double t_sandbox_ms = 0.0, t_native_ms = 0.0, t0;

  do {
    auto in_size = fread(in, 1, CHUNK, source);
    flush = feof(source) ? Z_FINISH : Z_NO_FLUSH;

    t0 = monotonic_ms();
    sandboxedStream->avail_in = in_size;
    if (ferror(source)) {
        (void)sandbox.invoke_sandbox_function(deflateEnd, sandboxedStream);
        return Z_ERRNO;
    }

    // malloc_in_sandbox(0) is undefined on at least wasm2c; when the input
    // size is an exact multiple of CHUNK the final Z_FINISH iteration has
    // in_size == 0. Pad to 1 so both backends behave the same.
    auto sandboxedIn = sandbox.malloc_in_sandbox<char>(in_size > 0 ? in_size : 1);
    if (in_size > 0) {
      rlbox::memcpy(sandbox, sandboxedIn, &in, in_size);
    }
    sandboxedStream->next_in = sandboxedIn;
    t_sandbox_ms += monotonic_ms() - t0;

    Bstrm.avail_in = fread(Bin, 1, CHUNK, Bsource);
    if (ferror(Bsource)) {
        (void)deflateEnd(&Bstrm);
        return Z_ERRNO;
    }
    Bflush = feof(Bsource) ? Z_FINISH : Z_NO_FLUSH;
    Bstrm.next_in = Bin;

    do {
      t0 = monotonic_ms();
      auto sandboxedOut = sandbox.malloc_in_sandbox<char>(CHUNK);
      sandboxedStream->avail_out = CHUNK;
      sandboxedStream->next_out = sandboxedOut;

      auto deflateRet = sandbox.invoke_sandbox_function(
        deflate, sandboxedStream, flush);
      verifiedDeflateRet = deflateRet.copy_and_verify([](int val){
          release_assert(val >= -6 && val <= 2, "Invalid ZLIB error code");
          return val;
      });

      assert(verifiedDeflateRet != Z_STREAM_ERROR);

      verifiedAvailOut = sandboxedStream->avail_out.copy_and_verify([](int val) {
          return val;
      });

      have = CHUNK - verifiedAvailOut;
      if(have > 0) {
        std::unique_ptr<char[]> uniqueOut =
        sandboxedOut.copy_and_verify_range([](std::unique_ptr<char[]> val) {
          return move(val);
        }, have);

        char * out = uniqueOut.release();
        if (fwrite(out, 1, have, dest) != have || ferror(dest)) {
          (void)sandbox.invoke_sandbox_function(deflateEnd, sandboxedStream);
          return Z_ERRNO;
        }
        sandbox.free_in_sandbox(sandboxedOut);
      }
      t_sandbox_ms += monotonic_ms() - t0;

      t0 = monotonic_ms();
      Bstrm.avail_out = CHUNK;
      Bstrm.next_out = Bout;
      Bret = deflate(&Bstrm, Bflush);
      assert(Bret != Z_STREAM_ERROR);
      Bhave = CHUNK - Bstrm.avail_out;
      if (fwrite(Bout, 1, Bhave, Bdest) != Bhave || ferror(Bdest)) {
        (void)deflateEnd(&Bstrm);
        return Z_ERRNO;
      }
      t_native_ms += monotonic_ms() - t0;

      assert(verifiedAvailOut == Bstrm.avail_out);

    } while (verifiedAvailOut == 0);

    t0 = monotonic_ms();
    sandbox.free_in_sandbox(sandboxedIn);

    auto verifiedAvailIn = sandboxedStream->avail_in.copy_and_verify([](int val){
        release_assert(val<=CHUNK, "Unread input cannot exceed CHUNK");
        return val;
    });
    assert(verifiedAvailIn == 0);
    t_sandbox_ms += monotonic_ms() - t0;

    assert(Bstrm.avail_in == 0);

    assert(Bflush == flush);

  } while (flush != Z_FINISH);

  assert(verifiedDeflateRet == Z_STREAM_END);
  assert(Bret == Z_STREAM_END);

  sandbox.destroy_sandbox();

  printf("SANDBOX_MS=%.3f NATIVE_MS=%.3f\n", t_sandbox_ms, t_native_ms);

  return 0;
}

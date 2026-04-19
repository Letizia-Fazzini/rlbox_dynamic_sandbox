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
// The fixed configuration line we need to use for the wasm2c sandbox.
// It specifies that all calls into the sandbox are resolved statically.
#define RLBOX_USE_STATIC_CALLS() rlbox_wasm2c_sandbox_lookup_symbol
// The rlbox wasm2c plugin requires that you provide the wasm2c module's name
#define RLBOX_WASM2C_MODULE_NAME zlib

// Include the produced header from wasm2c
#include "zlib.wasm.h"
#include "rlbox.hpp"
#include "rlbox_wasm2c_sandbox.hpp"
#include "zlib.h"
#include "zlib_structs.h"

using namespace rlbox;

rlbox_load_structs_from_library(zlib);

// Define base type for zlib using the wasm2c sandbox
RLBOX_DEFINE_BASE_TYPES_FOR(zlib, wasm2c);

// Forward declarations of wrapper functions (defined in zlib_wrapper.c, compiled into the wasm module).
extern "C" {
  int deflateInitWrapper(z_streamp strm, int level);
  int inflateInitWrapper(z_streamp strm);
}

int main(int argc, char const *argv[]) {

  // Declare and create a new sandbox
  rlbox_sandbox_zlib sandbox;
  sandbox.create_sandbox();

  //sandbox variables
  FILE* source = fopen("pi.txt","r");
  FILE* dest = fopen("compressed.txt", "w");
  int  flush;
  unsigned have;
  z_stream initStream;
  unsigned char in[CHUNK];
  //unsigned char out[CHUNK];

  int level = 2;
  if(argc>1) {
    level = std::stoi(argv[1]);
  }

  initStream.zalloc = Z_NULL;
  initStream.zfree = Z_NULL;
  initStream.opaque = Z_NULL;

  //sandbox init
  auto sandboxedStream = sandbox.malloc_in_sandbox<z_stream>();
  rlbox::memcpy(sandbox, sandboxedStream, &initStream, sizeof(z_stream));

  auto deflateInitRet = sandbox.invoke_sandbox_function(deflateInitWrapper, sandboxedStream, level);
  
  auto verifiedRet = deflateInitRet.copy_and_verify([](int val){
    release_assert(val >= -6 && val <= 2, "Invalid ZLIB error code");
    return val;
  });
  if (verifiedRet != Z_OK){
    return Z_ERRNO;
  }

  auto verifiedAvailOut = 0;
  auto verifiedDeflateRet = Z_OK;

  double t_sandbox_ms = 0.0, t0;

  /* compress until end of file */
  do {

    //sandbox
    auto in_size = fread(in, 1, CHUNK, source);
    flush = feof(source) ? Z_FINISH : Z_NO_FLUSH;

    t0 = monotonic_ms();
    sandboxedStream->avail_in = in_size;
    if (ferror(source)) {
        (void)sandbox.invoke_sandbox_function(deflateEnd, sandboxedStream);
        return Z_ERRNO;
    }

    // malloc_in_sandbox(0) aborts on wasm2c; when the input size is an exact
    // multiple of CHUNK the final Z_FINISH iteration has in_size == 0.
    auto sandboxedIn = sandbox.malloc_in_sandbox<char>(in_size > 0 ? in_size : 1);
    if (in_size > 0) {
      rlbox::memcpy(sandbox, sandboxedIn, &in, in_size);
    }
    sandboxedStream->next_in = sandboxedIn;
    t_sandbox_ms += monotonic_ms() - t0;

    /* run deflate() on input until output buffer not full, finish
        compression if all of source has been read in */
    do {

      //sandbox
      t0 = monotonic_ms();
      auto sandboxedOut = sandbox.malloc_in_sandbox<char>(CHUNK);
      sandboxedStream->avail_out = CHUNK;
      sandboxedStream->next_out = sandboxedOut;

      auto deflateRet = sandbox.invoke_sandbox_function(deflate, sandboxedStream, flush);
      verifiedDeflateRet = deflateRet.copy_and_verify([](int val){
          release_assert(val >= -6 && val <= 2, "Invalid ZLIB error code");
          return val;
      });

      assert(verifiedDeflateRet != Z_STREAM_ERROR);  /* state not clobbered */

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

    } while (verifiedAvailOut == 0);

    t0 = monotonic_ms();
    sandbox.free_in_sandbox(sandboxedIn);

    //sandbox
    auto verifiedAvailIn = sandboxedStream->avail_in.copy_and_verify([](int val){
        release_assert(val<=CHUNK, "Unread input cannot exceed CHUNK");
        return val;
    });
    assert(verifiedAvailIn == 0);     /* all input will be used */
    t_sandbox_ms += monotonic_ms() - t0;

  /* done when last data in file processed */
  } while (flush != Z_FINISH);

  //sandbox
  assert(verifiedDeflateRet == Z_STREAM_END); /* stream will be complete */

  // destroy sandbox
  sandbox.destroy_sandbox();

  printf("COMPRESSION_MS=%.3f\n", t_sandbox_ms);

  return 0;
}


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <time.h>

static double monotonic_ms()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

#define release_assert(cond, msg) if (!(cond)) { fputs(msg "\n", stderr); abort(); }

// We're going to use RLBox in a single-threaded environment.
#define RLBOX_SINGLE_THREADED_INVOCATIONS
// The meta-sandbox resolves symbols dynamically per-backend at dispatch time;
// do NOT define RLBOX_USE_STATIC_CALLS() here (see rlbox_meta_sandbox.hpp).
// The wasm2c module name and generated header must be set up before the meta
// include so the meta header (which re-includes rlbox_wasm2c_sandbox.hpp
// internally) can see them.
#define RLBOX_WASM2C_MODULE_NAME jpeg

// Include the produced header from wasm2c
#include "jpeg.wasm.h"
#include "rlbox.hpp"
#include "rlbox_meta_sandbox.hpp"
#include "turbojpeg.h"

using namespace rlbox;

// Define base types for libjpeg-turbo using the meta (adaptive) sandbox
RLBOX_DEFINE_BASE_TYPES_FOR(jpeg, meta);

int main(int argc, char const *argv[]) {

  //read in quality from stdin
  int quality = 50;
  if(argc>1) {
    quality = std::stoi(argv[1]);
  }

  // Declare and create a new sandbox (both process and wasm backends)
  rlbox_sandbox_jpeg sandbox;
  sandbox.create_sandbox(JPEG_PROCESS_WRAPPER_PATH);
  // Install the adaptive dispatch policy: explore each backend 3 times per
  // symbol, then route to whichever has the lower median latency.
  sandbox.get_sandbox_impl()->set_policy(rlbox::make_adaptive_policy());

  //put input stream inside sandbox as a flat packed pixel buffer
  FILE* source = fopen("test_data.txt", "r");
  int image_width, image_height, image_channels;
  fscanf(source, "%d %d %d", &image_width, &image_height, &image_channels);
  int row_stride = image_width * image_channels;

  auto sandboxSrc = sandbox.malloc_in_sandbox<unsigned char>(image_height * row_stride);
  for (int i = 0; i < image_height * row_stride; i++) {
    int val;
    fscanf(source, "%d", &val);
    sandboxSrc[i] = (unsigned char)val;
  }
  fclose(source);

  //declare output file
  FILE* destinationFile;
  if ((destinationFile = fopen("compressed.jpeg", "wb")) == NULL) {
    fprintf(stderr, "can't open output file\n");
    exit(1);
  }

  //set up output buffer pointers inside sandbox
  auto outBuffer = sandbox.malloc_in_sandbox<unsigned char*>();
  *outBuffer = nullptr;
  auto outSize   = sandbox.malloc_in_sandbox<unsigned long>();
  *outSize = 0;

  double t_start = monotonic_ms();
  auto tjHandle = sandbox.invoke_sandbox_function(tjInitCompress);

  auto compress_ret = sandbox.invoke_sandbox_function(
    tjCompress2,
    tjHandle,
    sandboxSrc,
    image_width,
    row_stride,
    image_height,
    TJPF_RGB,
    outBuffer,
    outSize,
    TJSAMP_444,
    quality,
    0
  );
  compress_ret.copy_and_verify([](int ret) {
    release_assert(ret == 0, "tjCompress2 failed");
    return ret;
  });

  sandbox.invoke_sandbox_function(tjDestroy, tjHandle);
  printf("COMPRESSION_MS=%.3f\n", monotonic_ms() - t_start);

  sandbox.free_in_sandbox(sandboxSrc);

  //copy data from sandbox buffer "outBuffer" to "compressed.jpeg"
  auto verifiedSizePtr = outSize.copy_and_verify([](std::unique_ptr<unsigned long> size) {
    release_assert(size != nullptr, "Output size ptr must not be null");
    release_assert(*size > 0, "Output size must be greater than zero");
    return size;
  });
  auto verifiedSize = (*verifiedSizePtr);

  auto localBuffer = (*outBuffer).copy_and_verify_range([](std::unique_ptr<unsigned char[]> val) {
    release_assert(val != nullptr, "Output buffer pointer must not be null");
    return move(val);
  }, verifiedSize);

  fwrite(localBuffer.get(), 1, verifiedSize, destinationFile);
  fclose(destinationFile);

  sandbox.free_in_sandbox(*outBuffer);
  sandbox.free_in_sandbox(outBuffer);
  sandbox.free_in_sandbox(outSize);

  // destroy sandbox
  sandbox.destroy_sandbox();

  return 0;
}


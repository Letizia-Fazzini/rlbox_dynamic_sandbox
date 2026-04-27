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
// The fixed configuration line we need to use for the wasm2c sandbox.
// It specifies that all calls into the sandbox are resolved statically.
#define RLBOX_USE_STATIC_CALLS() rlbox_wasm2c_sandbox_lookup_symbol
// The rlbox wasm2c plugin requires that you provide the wasm2c module's name
#define RLBOX_WASM2C_MODULE_NAME jpeg

// Include the produced header from wasm2c
#include "jpeg.wasm.h"
#include "rlbox.hpp"
#include "rlbox_wasm2c_sandbox.hpp"
#include "turbojpeg.h"

using namespace rlbox;

// Define base type for libjpeg-turbo using the wasm2c sandbox
RLBOX_DEFINE_BASE_TYPES_FOR(jpeg, wasm2c);

int main(int argc, char const *argv[]) {

  //read in quality from stdin
  int quality = 50;
  if(argc>1) {
    quality = std::stoi(argv[1]);
  }
  int num_datasets = 1;
  if(argc>2) {
    num_datasets = std::stoi(argv[2]);
  }
  int iters = 10;
  if(argc>3) {
    iters = std::stoi(argv[3]);
  }

  // Declare and create a new sandbox
  rlbox_sandbox_jpeg sandbox;
  sandbox.create_sandbox();

  for(int d = 1; d <= num_datasets; d++) {
    char filename[256];
    snprintf(filename, sizeof(filename), "test_data/test_data%d.txt", d);

    for(int it = 0; it < iters; it++) {
      fprintf(stderr, "[DBG] d=%d it=%d step=1: opening %s\n", d, it, filename); fflush(stderr);
      //put input stream inside sandbox as a flat packed pixel buffer
      FILE* source = fopen(filename, "r");
      int image_width, image_height, image_channels;
      fscanf(source, "%d %d %d", &image_width, &image_height, &image_channels);
      int row_stride = image_width * image_channels;
      fprintf(stderr, "[DBG] d=%d it=%d step=2: image %dx%dx%d, row_stride=%d\n", d, it, image_width, image_height, image_channels, row_stride); fflush(stderr);

      auto sandboxSrc = sandbox.malloc_in_sandbox<unsigned char>(image_height * row_stride);
      for (int i = 0; i < image_height * row_stride; i++) {
        int val;
        fscanf(source, "%d", &val);
        sandboxSrc[i] = (unsigned char)val;
      }
      fclose(source);
      fprintf(stderr, "[DBG] d=%d it=%d step=4: pixel data loaded\n", d, it); fflush(stderr);

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
      fprintf(stderr, "[DBG] d=%d it=%d step=5: outBuffer/outSize allocated\n", d, it); fflush(stderr);

      double t_start = monotonic_ms();
      fprintf(stderr, "[DBG] d=%d it=%d step=6: calling tjInitCompress\n", d, it); fflush(stderr);
      auto tjHandle = sandbox.invoke_sandbox_function(tjInitCompress);
      fprintf(stderr, "[DBG] d=%d it=%d step=7: tjInitCompress returned, tjHandle valid=%d\n", d, it, (bool)tjHandle); fflush(stderr);

      fprintf(stderr, "[DBG] d=%d it=%d step=8: calling tjCompress2\n", d, it); fflush(stderr);
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
      fprintf(stderr, "[DBG] d=%d it=%d step=9: tjCompress2 returned\n", d, it); fflush(stderr);
      compress_ret.copy_and_verify([](int ret) {
        release_assert(ret == 0, "tjCompress2 failed");
        return ret;
      });

      fprintf(stderr, "[DBG] d=%d it=%d step=10: calling tjDestroy\n", d, it); fflush(stderr);
      sandbox.invoke_sandbox_function(tjDestroy, tjHandle);
      printf("COMPRESSION_MS=%.3f\n", monotonic_ms() - t_start); fflush(stdout);
      fprintf(stderr, "[DBG] d=%d it=%d step=11: tjDestroy done, freeing sandboxSrc\n", d, it); fflush(stderr);

      sandbox.free_in_sandbox(sandboxSrc);
      fprintf(stderr, "[DBG] d=%d it=%d step=12: sandboxSrc freed\n", d, it); fflush(stderr);

      //copy data from sandbox buffer "outBuffer" to "compressed.jpeg"
      fprintf(stderr, "[DBG] d=%d it=%d step=13: copy_and_verify outSize\n", d, it); fflush(stderr);
      auto verifiedSizePtr = outSize.copy_and_verify([](std::unique_ptr<unsigned long> size) {
        release_assert(size != nullptr, "Output size ptr must not be null");
        release_assert(*size > 0, "Output size must be greater than zero");
        return size;
      });
      auto verifiedSize = (*verifiedSizePtr);
      fprintf(stderr, "[DBG] d=%d it=%d step=14: verifiedSize=%lu\n", d, it, verifiedSize); fflush(stderr);

      fprintf(stderr, "[DBG] d=%d it=%d step=15: copy_and_verify_range outBuffer\n", d, it); fflush(stderr);
      auto localBuffer = (*outBuffer).copy_and_verify_range([](std::unique_ptr<unsigned char[]> val) {
        release_assert(val != nullptr, "Output buffer pointer must not be null");
        return move(val);
      }, verifiedSize);
      fprintf(stderr, "[DBG] d=%d it=%d step=16: copy_and_verify_range done\n", d, it); fflush(stderr);

      fwrite(localBuffer.get(), 1, verifiedSize, destinationFile);
      fclose(destinationFile);
      fprintf(stderr, "[DBG] d=%d it=%d step=17: fwrite done, freeing sandbox buffers\n", d, it); fflush(stderr);

      sandbox.free_in_sandbox(*outBuffer);
      fprintf(stderr, "[DBG] d=%d it=%d step=18: *outBuffer freed\n", d, it); fflush(stderr);
      sandbox.free_in_sandbox(outBuffer);
      fprintf(stderr, "[DBG] d=%d it=%d step=19: outBuffer freed\n", d, it); fflush(stderr);
      sandbox.free_in_sandbox(outSize);
      fprintf(stderr, "[DBG] d=%d it=%d step=20: outSize freed, iteration complete\n", d, it); fflush(stderr);
    }
  }

  // destroy sandbox
  sandbox.destroy_sandbox();

  return 0;
}


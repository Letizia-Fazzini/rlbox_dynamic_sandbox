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

#define RLBOX_SINGLE_THREADED_INVOCATIONS

// Process sandbox resolves symbols dynamically via dlsym; no
// RLBOX_USE_STATIC_CALLS().

#include "rlbox.hpp"
#include "rlbox_process_sandbox.hpp"
#include "turbojpeg.h"

using namespace rlbox;

RLBOX_DEFINE_BASE_TYPES_FOR(jpeg, process);

int main(int argc, char const *argv[]) {

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

  rlbox_sandbox_jpeg sandbox;
  sandbox.create_sandbox(JPEG_PROCESS_WRAPPER_PATH);

  for(int d = 1; d <= num_datasets; d++) {
    char filename[256];
    snprintf(filename, sizeof(filename), "test_data/test_data%d.txt", d);

    for(int it = 0; it < iters; it++) {
      FILE* source = fopen(filename, "r");
      int image_width, image_height, image_channels;
      fscanf(source, "%d %d %d", &image_width, &image_height, &image_channels);
      int row_stride = image_width * image_channels;

      //set up output buffer pointers inside sandbox.  We pre-allocate the
      //JPEG output buffer ourselves and pass TJFLAG_NOREALLOC so
      //libjpeg-turbo won't replace it with an internal tjAlloc'd buffer.
      auto maxSize = sandbox.invoke_sandbox_function(tjBufSize, image_width, image_height, TJSAMP_444);
      auto verifiedMaxSize = maxSize.copy_and_verify([](unsigned long ret) {
        release_assert(ret != 0, "max size cannot be 0");
        return ret;
      });
      auto sandboxOut = sandbox.malloc_in_sandbox<unsigned char>(verifiedMaxSize);

      auto outBuffer = sandbox.malloc_in_sandbox<unsigned char*>();
      *outBuffer = sandboxOut;
      auto outSize   = sandbox.malloc_in_sandbox<unsigned long>();
      *outSize = verifiedMaxSize;

      auto sandboxSrc = sandbox.malloc_in_sandbox<unsigned char>(image_height * row_stride);
      for (int i = 0; i < image_height * row_stride; i++) {
        int val;
        fscanf(source, "%d", &val);
        sandboxSrc[i] = (unsigned char)val;
      }
      fclose(source);

      FILE* destinationFile;
      if ((destinationFile = fopen("compressed.jpeg", "wb")) == NULL) {
        fprintf(stderr, "can't open output file\n");
        exit(1);
      }

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
        TJFLAG_NOREALLOC
      );
      compress_ret.copy_and_verify([](int ret) {
        release_assert(ret == 0, "tjCompress2 failed");
        return ret;
      });

      sandbox.invoke_sandbox_function(tjDestroy, tjHandle);
      printf("COMPRESSION_MS=%.3f\n", monotonic_ms() - t_start); fflush(stdout);

      sandbox.free_in_sandbox(sandboxSrc);

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

      sandbox.free_in_sandbox(sandboxOut);
      sandbox.free_in_sandbox(outBuffer);
      sandbox.free_in_sandbox(outSize);
    }
  }

  sandbox.destroy_sandbox();

  return 0;
}

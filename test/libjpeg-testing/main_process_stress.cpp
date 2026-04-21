// Bulk-scanlines libjpeg compression via rlbox_process_sandbox —
// counterpart to main_stress.cpp (wasm2c). Same shape: stage the entire
// image in the sandbox, then call jpeg_write_scanlines(cinfo, sandboxSource,
// image_height) once instead of looping per-row. With a single invoke the
// per-call fork() is paid once, which is the regime where process can
// plausibly match or beat wasm2c (no bounds-checked memory accesses, no
// 4-byte struct layout, native fpu).
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

#include "rlbox.hpp"
#include "rlbox_process_sandbox.hpp"
#include "jpeglib.h"
#include "jpeg_structs.h"

using namespace rlbox;

rlbox_load_structs_from_library(jpeg);
RLBOX_DEFINE_BASE_TYPES_FOR(jpeg, process);

int main(int argc, char const *argv[]) {
  int quality = 75;
  if (argc > 1) quality = std::stoi(argv[1]);

  rlbox_sandbox_jpeg sandbox;
  sandbox.create_sandbox(JPEG_PROCESS_WRAPPER_PATH);

  FILE* source = fopen("rgb_grid.txt", "r");
  release_assert(source, "could not open rgb_grid.txt");

  int image_width, image_height, image_channels;
  fscanf(source, "%d %d %d", &image_width, &image_height, &image_channels);
  int row_stride = image_width * image_channels * sizeof(JSAMPLE);

  auto sandboxSource = sandbox.malloc_in_sandbox<JSAMPROW>(image_height);
  for (int i = 0; i < image_height; i++) {
    JSAMPLE* row = (JSAMPLE*)malloc(row_stride);
    for (int j = 0; j < row_stride; j++) {
      int val;
      fscanf(source, "%d", &val);
      row[j] = (JSAMPLE)val;
    }
    auto sandboxedRow = sandbox.malloc_in_sandbox<JSAMPLE>(image_width * 3);
    rlbox::memcpy(sandbox, sandboxedRow, row, row_stride);
    free(row);
    sandboxSource[i] = sandboxedRow;
  }
  fclose(source);

  auto cinfo = sandbox.malloc_in_sandbox<jpeg_compress_struct>();
  auto jerr  = sandbox.malloc_in_sandbox<jpeg_error_mgr>();

  auto returnedErr = sandbox.invoke_sandbox_function(jpeg_std_error, jerr);
  cinfo->err = returnedErr;

  sandbox.invoke_sandbox_function(jpeg_CreateCompress, cinfo,
      JPEG_LIB_VERSION,
      (size_t)sizeof(struct jpeg_compress_struct));

  FILE* destinationFile = fopen("compressed.jpeg", "wb");
  release_assert(destinationFile, "can't open output file");

  auto outBuffer = sandbox.malloc_in_sandbox<unsigned char*>();
  *outBuffer = nullptr;
  auto outSize  = sandbox.malloc_in_sandbox<unsigned long>();
  *outSize = 0;
  sandbox.invoke_sandbox_function(jpeg_mem_dest, cinfo, outBuffer, outSize);

  cinfo->image_width       = image_width;
  cinfo->image_height      = image_height;
  cinfo->input_components  = image_channels;
  cinfo->in_color_space    = (int)JCS_RGB;

  sandbox.invoke_sandbox_function(jpeg_set_defaults, cinfo);
  sandbox.invoke_sandbox_function(jpeg_set_quality, cinfo, quality, true);

  double t0 = monotonic_ms();

  sandbox.invoke_sandbox_function(jpeg_start_compress, cinfo, TRUE);

  (void) sandbox.invoke_sandbox_function(
      jpeg_write_scanlines, cinfo, sandboxSource, (JDIMENSION)image_height);

  auto verifiedNextScanline = cinfo->next_scanline.copy_and_verify(
      [image_height](JDIMENSION v){
        release_assert(v == (JDIMENSION)image_height,
                       "bulk jpeg_write_scanlines did not process every row");
        return v;
      });
  (void)verifiedNextScanline;

  sandbox.invoke_sandbox_function(jpeg_finish_compress, cinfo);

  double t_sandbox_ms = monotonic_ms() - t0;

  sandbox.invoke_sandbox_function(jpeg_destroy_compress, cinfo);

  auto verifiedSizePtr = outSize.copy_and_verify([](std::unique_ptr<unsigned long> size){
    release_assert(size != nullptr, "Output size ptr must not be null");
    release_assert(*size > 0, "Output size must be greater than zero");
    return size;
  });
  auto verifiedSize = (*verifiedSizePtr);

  auto localBuffer = (*outBuffer).copy_and_verify_range(
      [](std::unique_ptr<unsigned char[]> val){
        release_assert(val != nullptr, "Output buffer pointer must not be null");
        return std::move(val);
      }, verifiedSize);

  fwrite(localBuffer.get(), 1, verifiedSize, destinationFile);
  fclose(destinationFile);

  sandbox.free_in_sandbox(sandboxSource);
  sandbox.destroy_sandbox();

  printf("COMPRESSION_MS=%.3f\n", t_sandbox_ms);
  return 0;
}

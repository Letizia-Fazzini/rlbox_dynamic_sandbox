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

// Process sandbox uses dynamic symbol resolution via dlsym in the child —
// no RLBOX_USE_STATIC_CALLS()

#include "rlbox.hpp"
#include "rlbox_process_sandbox.hpp"
#include "jpeglib.h"
#include "jpeg_structs.h"

using namespace rlbox;

rlbox_load_structs_from_library(jpeg);

RLBOX_DEFINE_BASE_TYPES_FOR(jpeg, process);

int main(int argc, char const *argv[]) {

  //read in quality from stdin
  int quality = 50;
  if(argc>1) {
    quality = std::stoi(argv[1]);
  }

  int batch_size = 1;
  if(argc>2) {
    batch_size = std::stoi(argv[2]);
    if(batch_size < 1) batch_size = 1;
  }

  double t_stage = monotonic_ms();
  rlbox_sandbox_jpeg sandbox;
  sandbox.create_sandbox(JPEG_PROCESS_WRAPPER_PATH);

  //put input stream inside sandbox
  t_stage = monotonic_ms();
  FILE* source = fopen("test_data.txt", "r");
  int image_width, image_height, image_channels;
  fscanf(source, "%d %d %d", &image_width, &image_height, &image_channels);
  int row_stride = image_width * image_channels * sizeof(JSAMPLE);

  auto sandboxSource = sandbox.malloc_in_sandbox<JSAMPROW>(image_height);

  for(int i = 0; i < image_height; i++) {

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

  //allocate libjpeg objects in sandbox
  t_stage = monotonic_ms();
  auto cinfo = sandbox.malloc_in_sandbox<jpeg_compress_struct>();
  auto jerr = sandbox.malloc_in_sandbox<jpeg_error_mgr>();

  auto returnedErr = sandbox.invoke_sandbox_function(jpeg_std_error, jerr);
  cinfo->err = returnedErr;

  // Pass the correct sizeof to CreateCompress; also log what we're passing
  size_t struct_size = sizeof(struct jpeg_compress_struct);
  sandbox.invoke_sandbox_function(jpeg_CreateCompress, cinfo, JPEG_LIB_VERSION, struct_size);

  //declare output file
  FILE* destinationFile;
  if ((destinationFile = fopen("compressed.jpeg", "wb")) == NULL) {
    fprintf(stderr, "can't open output file\n");
    exit(1);
  }

  //set up output stream inside sandbox
  auto outBuffer = sandbox.malloc_in_sandbox<unsigned char*>();
  *outBuffer = nullptr;
  auto outSize   = sandbox.malloc_in_sandbox<unsigned long>();
  *outSize = 0;
  sandbox.invoke_sandbox_function(jpeg_mem_dest, cinfo, outBuffer, outSize);

  //set parmeters for compression
  cinfo->image_width = image_width;
  cinfo->image_height = image_height;
  cinfo->input_components = image_channels;
  cinfo->in_color_space = (int)JCS_RGB;

  sandbox.invoke_sandbox_function(jpeg_set_defaults, cinfo);
  sandbox.invoke_sandbox_function(jpeg_set_quality, cinfo, quality, true);

  double t_sandbox_ms = 0.0, t0;
  t0 = monotonic_ms();

  //begin compression cycle
  double t_sandbox_ms = 0.0, t0;

  t0 = monotonic_ms();
  sandbox.invoke_sandbox_function(jpeg_start_compress, cinfo, TRUE);
  t_sandbox_ms += monotonic_ms() - t0;

  //write jpeg data to buffer in batches
  auto batchRows = sandbox.malloc_in_sandbox<JSAMPROW>(batch_size);

  auto verifiedNextScanline = cinfo->next_scanline.copy_and_verify([](int lines){
    release_assert(lines>=0, "Number of scanlines read so far must be non-negative");
    return lines;
  });
  auto verifiedImageHeight = cinfo->image_height.copy_and_verify([verifiedNextScanline](int height){
    release_assert(height>=0, "Image height must be non-negative");
    release_assert(height>=verifiedNextScanline, "Cannot have written more scanlines than total height");
    return height;
  });

  while (verifiedNextScanline < verifiedImageHeight) {

    int remaining = verifiedImageHeight - verifiedNextScanline;
    int this_batch = (remaining < batch_size) ? remaining : batch_size;
    
    for (int b = 0; b < this_batch; b++) {
      batchRows[b] = sandboxSource[verifiedNextScanline + b];
    }

    t0 = monotonic_ms();
    auto write_ret = sandbox.invoke_sandbox_function(jpeg_write_scanlines, cinfo, batchRows, this_batch);
    double batch_ms = monotonic_ms() - t0;
    t_sandbox_ms += batch_ms;

    auto retval = write_ret.copy_and_verify([](unsigned int v) { return v; });

    verifiedNextScanline = cinfo->next_scanline.copy_and_verify([](int lines){
      release_assert(lines>=0, "Number of scanlines read so far must be non-negative");
      return lines;
    });
    verifiedImageHeight = cinfo->image_height.copy_and_verify([verifiedNextScanline](int height){
      release_assert(height>=0, "Image height must be non-negative");
      release_assert(height>=verifiedNextScanline, "Cannot have written more scanlines than total height");
      return height;
    });

  }

  // complete compression cycle
  t0 = monotonic_ms();
  sandbox.invoke_sandbox_function(jpeg_finish_compress, cinfo);
  t_sandbox_ms += monotonic_ms() - t0;

  printf("COMPRESSION_MS=%.3f\n", t_sandbox_ms);

  t_sandbox_ms += monotonic_ms() - t0;

  //destroy jpeg object
  sandbox.invoke_sandbox_function(jpeg_destroy_compress, cinfo);

  //free sandbox resources
  sandbox.free_in_sandbox(sandboxSource);
  sandbox.free_in_sandbox(batchRows);

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

  // destroy sandbox
  sandbox.destroy_sandbox();

  printf("COMPRESSION_MS=%.3f\n", t_sandbox_ms);

  return 0;
}

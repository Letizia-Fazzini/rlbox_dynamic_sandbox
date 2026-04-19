#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

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
#include "jpeglib.h"
#include "jpeg_structs.h"

using namespace rlbox;

rlbox_load_structs_from_library(jpeg);

// Define base type for libjpeg-turbo using the wasm2c sandbox
RLBOX_DEFINE_BASE_TYPES_FOR(jpeg, wasm2c);

int main(int argc, char const *argv[]) {

  //read in quality from stdin
  int quality = 50;
  if(argc>1) {
    quality = std::stoi(argv[1]);
  }  

  // Declare and create a new sandbox
  rlbox_sandbox_jpeg sandbox;
  sandbox.create_sandbox();

  //put input stream inside sandbox
  FILE* source = fopen("rgb_grid.txt", "r");
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
  auto cinfo = sandbox.malloc_in_sandbox<jpeg_compress_struct>();
  auto jerr = sandbox.malloc_in_sandbox<jpeg_error_mgr>();  

  auto returnedErr = sandbox.invoke_sandbox_function(jpeg_std_error, jerr);
  cinfo->err = returnedErr;

  sandbox.invoke_sandbox_function(jpeg_CreateCompress, cinfo, JPEG_LIB_VERSION, (size_t)sizeof(rlbox::Sbx_jpeg_jpeg_compress_struct<rlbox_wasm2c_sandbox>));

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

  //begin compression cycle
  sandbox.invoke_sandbox_function(jpeg_start_compress, cinfo, TRUE);

  //JSAMPROW row_pointer[1];
  auto currentSandboxRow = sandbox.malloc_in_sandbox<JSAMPROW>();
  auto verifiedNextScanline = cinfo->next_scanline.copy_and_verify([](int lines){
    release_assert(lines>=0, "Number of scanlines read so far must be non-negative");
    return lines;
  });
  auto verifiedImageHeight = cinfo->image_height.copy_and_verify([verifiedNextScanline](int height){
    release_assert(height>=0, "Image height must be non-negative");
    release_assert(height>=verifiedNextScanline, "Cannot have written more scanlines than total height");
    return height;
  });

  //write jpeg data to buffer
  while (verifiedNextScanline < verifiedImageHeight) {

    currentSandboxRow[0] = sandboxSource[verifiedNextScanline];
    (void) sandbox.invoke_sandbox_function(jpeg_write_scanlines, cinfo, currentSandboxRow, 1);

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
  sandbox.invoke_sandbox_function(jpeg_finish_compress, cinfo);

  //destroy jpeg object
  sandbox.invoke_sandbox_function(jpeg_destroy_compress, cinfo);

  //free sandbox resources
  sandbox.free_in_sandbox(sandboxSource);
  sandbox.free_in_sandbox(currentSandboxRow);

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

  return 0;
}


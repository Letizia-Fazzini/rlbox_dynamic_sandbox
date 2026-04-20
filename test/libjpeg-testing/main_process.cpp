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

extern "C" void jpeg_report_struct_layout(void);
extern "C" void jpeg_diag_check_cinfo(struct jpeg_compress_struct *);

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

  fprintf(stderr, "[progress] quality=%d batch_size=%d\n", quality, batch_size);

  // Declare and create a new sandbox
  fprintf(stderr, "[progress] creating sandbox...\n");
  double t_stage = monotonic_ms();
  rlbox_sandbox_jpeg sandbox;
  sandbox.create_sandbox(JPEG_PROCESS_WRAPPER_PATH);
  fprintf(stderr, "[progress] sandbox created in %.1f ms\n", monotonic_ms() - t_stage);

  //put input stream inside sandbox
  fprintf(stderr, "[progress] reading input file...\n");
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
  fprintf(stderr, "[progress] input loaded in %.1f ms\n", monotonic_ms() - t_stage);

  //allocate libjpeg objects in sandbox
  fprintf(stderr, "[progress] setting up libjpeg objects in sandbox...\n");
  t_stage = monotonic_ms();
  auto cinfo = sandbox.malloc_in_sandbox<jpeg_compress_struct>();
  auto jerr = sandbox.malloc_in_sandbox<jpeg_error_mgr>();

  auto returnedErr = sandbox.invoke_sandbox_function(jpeg_std_error, jerr);
  cinfo->err = returnedErr;

  /*
  // ---- Layout diagnostics (parent side) ----
  fprintf(stderr, "[diag] JPEG_LIB_VERSION (parent header) = %d\n", JPEG_LIB_VERSION);
  fprintf(stderr, "[diag] sizeof(jpeg_compress_struct) in parent = %zu\n",
          sizeof(struct jpeg_compress_struct));
  fprintf(stderr, "[diag] offsetof(jpeg_compress_struct, global_state) in parent = %zu\n",
          offsetof(struct jpeg_compress_struct, global_state));
  fprintf(stderr, "[diag] sizeof(jpeg_error_mgr) in parent = %zu\n",
          sizeof(struct jpeg_error_mgr));

  
  // Check cinfo BEFORE jpeg_CreateCompress (should be uninitialized/garbage)
  {
    auto* raw = reinterpret_cast<struct jpeg_compress_struct*>(cinfo.UNSAFE_unverified());
    fprintf(stderr, "[diag] cinfo raw ptr (UNSAFE_unverified) = %p\n", (void*)raw);
    fprintf(stderr, "[diag] BEFORE CreateCompress: global_state=%d, err=%p\n",
            raw->global_state, (void*)raw->err);
    // Dump first 32 bytes of cinfo to catch offset surprises
    unsigned char* bytes = reinterpret_cast<unsigned char*>(raw);
    fprintf(stderr, "[diag] BEFORE CreateCompress raw bytes[0..31]:");
    for (int i = 0; i < 32; i++) fprintf(stderr, " %02x", bytes[i]);
    fprintf(stderr, "\n");
  }
    */

  // Pass the correct sizeof to CreateCompress; also log what we're passing
  size_t parent_sizeof = sizeof(struct jpeg_compress_struct);
  fprintf(stderr, "[diag] calling jpeg_CreateCompress(cinfo, version=%d, size=%zu)\n",
          JPEG_LIB_VERSION, parent_sizeof);
  sandbox.invoke_sandbox_function(jpeg_CreateCompress, cinfo, JPEG_LIB_VERSION, parent_sizeof);
  fprintf(stderr, "[diag] jpeg_CreateCompress returned\n");

  /*
  // Check cinfo AFTER jpeg_CreateCompress (should have global_state=100=CSTATE_START)
  {
    auto* raw = reinterpret_cast<struct jpeg_compress_struct*>(cinfo.UNSAFE_unverified());
    fprintf(stderr, "[diag] AFTER CreateCompress: global_state=%d (expect 100=CSTATE_START)\n",
            raw->global_state);
    fprintf(stderr, "[diag] AFTER CreateCompress: err=%p, image_width=%u\n",
            (void*)raw->err, raw->image_width);
    // Also read global_state via RLBox tainted accessor (not raw pointer)
    int tainted_gs = cinfo->global_state.copy_and_verify([](int v){ return v; });
    fprintf(stderr, "[diag] AFTER CreateCompress: global_state via RLBox tainted accessor = %d\n",
            tainted_gs);
    // Dump raw bytes again to see what actually changed at the pointer level
    unsigned char* bytes = reinterpret_cast<unsigned char*>(raw);
    fprintf(stderr, "[diag] AFTER CreateCompress raw bytes[0..31]:");
    for (int i = 0; i < 32; i++) fprintf(stderr, " %02x", bytes[i]);
    fprintf(stderr, "\n");
    // Show the byte range covering global_state offset
    size_t gs_off = offsetof(struct jpeg_compress_struct, global_state);
    fprintf(stderr, "[diag] bytes at global_state offset %zu: %02x %02x %02x %02x\n",
            gs_off, bytes[gs_off], bytes[gs_off+1], bytes[gs_off+2], bytes[gs_off+3]);
  }
            */

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
  fprintf(stderr, "[progress] libjpeg setup done in %.1f ms\n", monotonic_ms() - t_stage);

  //begin compression cycle
  double t_sandbox_ms = 0.0, t0;

  fprintf(stderr, "[progress] calling jpeg_start_compress...\n");
  t0 = monotonic_ms();
  sandbox.invoke_sandbox_function(jpeg_start_compress, cinfo, TRUE);
  t_sandbox_ms += monotonic_ms() - t0;
  fprintf(stderr, "[progress] jpeg_start_compress done in %.1f ms\n", monotonic_ms() - t0);

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
    fprintf(stderr, "[progress] jpeg_write_scanlines rows %d-%d / %d...\n",
            verifiedNextScanline, verifiedNextScanline + this_batch - 1, verifiedImageHeight);

    for (int b = 0; b < this_batch; b++) {
      batchRows[b] = sandboxSource[verifiedNextScanline + b];
    }

    t0 = monotonic_ms();
    auto write_ret = sandbox.invoke_sandbox_function(jpeg_write_scanlines, cinfo, batchRows, this_batch);
    double batch_ms = monotonic_ms() - t0;
    t_sandbox_ms += batch_ms;

    auto retval = write_ret.copy_and_verify([](unsigned int v) { return v; });
    fprintf(stderr, "[diag] jpeg_write_scanlines returned %u (requested %d)\n",
            retval, this_batch);

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
  fprintf(stderr, "[progress] calling jpeg_finish_compress...\n");
  t0 = monotonic_ms();
  sandbox.invoke_sandbox_function(jpeg_finish_compress, cinfo);
  t_sandbox_ms += monotonic_ms() - t0;
  fprintf(stderr, "[progress] jpeg_finish_compress done in %.1f ms\n", monotonic_ms() - t0);

  printf("COMPRESSION_MS=%.3f\n", t_sandbox_ms);

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

  return 0;
}

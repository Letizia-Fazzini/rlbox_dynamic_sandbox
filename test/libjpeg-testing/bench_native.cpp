#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <string>
#include <time.h>

#include "jpeglib.h"

static double monotonic_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

int main(int argc, char const *argv[]) {

  // read in quality from command line
  int quality = 50;
  if (argc > 1) {
    quality = std::stoi(argv[1]);
  }

  int batch_size = 1;
  if (argc > 2) {
    batch_size = std::stoi(argv[2]);
    if (batch_size < 1) batch_size = 1;
  }

  // read image from test_data.txt
  FILE* source = fopen("test_data.txt", "r");
  if (!source) {
    fprintf(stderr, "can't open test_data.txt\n");
    exit(1);
  }
  int image_width, image_height, image_channels;
  fscanf(source, "%d %d %d", &image_width, &image_height, &image_channels);
  int row_stride = image_width * image_channels * sizeof(JSAMPLE);

  // read all rows into a native buffer
  JSAMPROW* imageData = (JSAMPROW*)malloc(image_height * sizeof(JSAMPROW));
  for (int i = 0; i < image_height; i++) {
    JSAMPLE* row = (JSAMPLE*)malloc(row_stride);
    for (int j = 0; j < row_stride; j++) {
      int val;
      fscanf(source, "%d", &val);
      row[j] = (JSAMPLE)val;
    }
    imageData[i] = row;
  }
  fclose(source);

  // allocate libjpeg objects natively on the stack
  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);

  // open output file and use stdio destination (no intermediate buffer needed)
  FILE* destinationFile;
  if ((destinationFile = fopen("compressed.jpeg", "wb")) == NULL) {
    fprintf(stderr, "can't open output file\n");
    exit(1);
  }
  jpeg_stdio_dest(&cinfo, destinationFile);

  // set parameters for compression
  cinfo.image_width = image_width;
  cinfo.image_height = image_height;
  cinfo.input_components = image_channels;
  cinfo.in_color_space = JCS_RGB;

  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);

  // compress scanline by scanline
  double elapsed_ms = 0.0;

  double t0 = monotonic_ms();
  jpeg_start_compress(&cinfo, TRUE);
  elapsed_ms += monotonic_ms() - t0;

  while (cinfo.next_scanline < cinfo.image_height) {
    int remaining = cinfo.image_height - cinfo.next_scanline;
    int this_batch = (remaining < batch_size) ? remaining : batch_size;
    JSAMPROW* batch_ptr = &imageData[cinfo.next_scanline];
    t0 = monotonic_ms();
    jpeg_write_scanlines(&cinfo, batch_ptr, this_batch);
    elapsed_ms += monotonic_ms() - t0;
  }

  t0 = monotonic_ms();
  jpeg_finish_compress(&cinfo);
  elapsed_ms += monotonic_ms() - t0;

  jpeg_destroy_compress(&cinfo);
  fclose(destinationFile);

  printf("COMPRESSION_MS=%.3f\n", elapsed_ms);

  // free image data
  for (int i = 0; i < image_height; i++) {
    free(imageData[i]);
  }
  free(imageData);

  return 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <string>
#include <time.h>

#include "turbojpeg.h"

static double monotonic_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

int main(int argc, char const *argv[]) {

  int quality = 50;
  if (argc > 1) {
    quality = std::stoi(argv[1]);
  }
  int num_datasets = 1;
  if (argc > 2) {
    num_datasets = std::stoi(argv[2]);
  }
  int iters = 10;
  if (argc > 3) {
    iters = std::stoi(argv[3]);
  }

  for(int d = 1; d <= num_datasets; d++) {
    char filename[256];
    snprintf(filename, sizeof(filename), "test_data/test_data%d.txt", d);

    for(int it = 0; it < iters; it++) {
      FILE* source = fopen(filename, "r");
      if (!source) {
        fprintf(stderr, "can't open %s\n", filename);
        exit(1);
      }
      int image_width, image_height, image_channels;
      fscanf(source, "%d %d %d", &image_width, &image_height, &image_channels);
      int row_stride = image_width * image_channels;

      unsigned char* imageData = (unsigned char*)malloc(image_height * row_stride);
      for (int i = 0; i < image_height * row_stride; i++) {
        int val;
        fscanf(source, "%d", &val);
        imageData[i] = (unsigned char)val;
      }
      fclose(source);

      double t_start = monotonic_ms();
      tjhandle tjHandle = tjInitCompress();
      if (!tjHandle) {
        fprintf(stderr, "tjInitCompress failed: %s\n", tjGetErrorStr());
        exit(1);
      }

      unsigned char* outBuffer = nullptr;
      unsigned long outSize = 0;

      int ret = tjCompress2(
        tjHandle,
        imageData,
        image_width,
        row_stride,
        image_height,
        TJPF_RGB,
        &outBuffer,
        &outSize,
        TJSAMP_444,
        quality,
        0
      );
      if (ret != 0) {
        fprintf(stderr, "tjCompress2 failed: %s\n", tjGetErrorStr());
        exit(1);
      }

      tjDestroy(tjHandle);
      printf("COMPRESSION_MS=%.3f\n", monotonic_ms() - t_start);
      free(imageData);

      FILE* destinationFile = fopen("compressed.jpeg", "wb");
      if (!destinationFile) {
        fprintf(stderr, "can't open output file\n");
        exit(1);
      }
      fwrite(outBuffer, 1, outSize, destinationFile);
      fclose(destinationFile);

      tjFree(outBuffer);
    }
  }

  return 0;
}
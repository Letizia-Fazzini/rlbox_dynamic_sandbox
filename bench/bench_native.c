/*
 * Standalone native-zlib reference for the sandbox benchmark.
 * Mirrors the compression loop in zlib-testing's main.cpp so the numbers
 * are comparable: CHUNK=16384, fread-deflate-fwrite in a tight loop.
 *
 * Usage: ./bench_native <input_path> <level>
 * Output: "NATIVE_MS=<elapsed>\n"
 *
 * The wall-clock measurement brackets only the compression work — file
 * open and setup happen outside, matching the sandbox mains' accumulators.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#define CHUNK 16384

static double monotonic_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

int main(int argc, char const* argv[])
{
  if (argc < 3) {
    fprintf(stderr, "usage: %s <input_path> <level>\n", argv[0]);
    return 2;
  }
  const char* input_path = argv[1];
  int level = atoi(argv[2]);

  FILE* source = fopen(input_path, "rb");
  FILE* dest = fopen("compressed_native.bin", "wb");
  if (!source || !dest) {
    perror("fopen");
    return 1;
  }

  z_stream strm;
  strm.zalloc = Z_NULL;
  strm.zfree = Z_NULL;
  strm.opaque = Z_NULL;
  int ret = deflateInit(&strm, level);
  if (ret != Z_OK)
    return Z_ERRNO;

  unsigned char in[CHUNK];
  unsigned char out[CHUNK];
  int flush;
  double elapsed_ms = 0.0;

  do {
    strm.avail_in = fread(in, 1, CHUNK, source);
    if (ferror(source)) {
      (void)deflateEnd(&strm);
      return Z_ERRNO;
    }
    flush = feof(source) ? Z_FINISH : Z_NO_FLUSH;
    strm.next_in = in;

    do {
      double t0 = monotonic_ms();
      strm.avail_out = CHUNK;
      strm.next_out = out;
      ret = deflate(&strm, flush);
      assert(ret != Z_STREAM_ERROR);
      unsigned have = CHUNK - strm.avail_out;
      if (fwrite(out, 1, have, dest) != have || ferror(dest)) {
        (void)deflateEnd(&strm);
        return Z_ERRNO;
      }
      elapsed_ms += monotonic_ms() - t0;
    } while (strm.avail_out == 0);

    assert(strm.avail_in == 0);

  } while (flush != Z_FINISH);

  assert(ret == Z_STREAM_END);
  deflateEnd(&strm);
  fclose(source);
  fclose(dest);

  printf("NATIVE_MS=%.3f\n", elapsed_ms);
  return 0;
}

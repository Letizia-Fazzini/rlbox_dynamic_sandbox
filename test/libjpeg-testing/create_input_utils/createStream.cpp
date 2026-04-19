// Standalone utility: reads sample_image.png and writes rgb_grid.txt
// Compile independently (not part of the CMake project):
//   g++ createStream.cpp -o createStream
// Requires: stb_image.h in the same directory (https://github.com/nothings/stb)
// Run:
//   ./createStream
//
// Output format of rgb_grid.txt:
//   Line 1: <width> <height>
//   Lines 2..(height+1): R G B R G B ... (one line per image row, space-separated)

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <cstdio>
#include <cstdlib>

int main() {
    const char* input_path  = "sample_image.png";
    const char* output_path = "rgb_grid.txt";

    int width, height, channels;
    // Force 3 channels (RGB) regardless of source format
    int REQ_CHANNELS = 3;
    unsigned char* data = stbi_load(input_path, &width, &height, &channels, REQ_CHANNELS);
    if (!data) {
        fprintf(stderr, "Error: could not load '%s': %s\n",
                input_path, stbi_failure_reason());
        return 1;
    }

    FILE* out = fopen(output_path, "w");
    if (!out) {
        fprintf(stderr, "Error: could not open '%s' for writing\n", output_path);
        stbi_image_free(data);
        return 1;
    }

    fprintf(out, "%d %d %d\n", width, height, REQ_CHANNELS);

    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            int idx = (row * width + col) * 3;
            if (col > 0) fputc(' ', out);
            fprintf(out, "%d %d %d",
                    (int)data[idx],
                    (int)data[idx + 1],
                    (int)data[idx + 2]);
        }
        fputc('\n', out);
    }

    fclose(out);
    stbi_image_free(data);

    printf("Wrote %dx%d image to '%s'\n", width, height, output_path);
    return 0;
}

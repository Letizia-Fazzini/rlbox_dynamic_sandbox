#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <jpeglib.h>

// Diagnostic: inspect cinfo from the child process side
void jpeg_diag_check_cinfo(struct jpeg_compress_struct *cinfo)
{
  fprintf(stderr, "[sandbox-child] jpeg_diag_check_cinfo called, cinfo=%p\n", (void*)cinfo);
  if (!cinfo) {
    fprintf(stderr, "[sandbox-child]   cinfo is NULL!\n");
    fflush(stderr);
    return;
  }
  fprintf(stderr, "[sandbox-child]   global_state     = %d\n", cinfo->global_state);
  fprintf(stderr, "[sandbox-child]   image_width      = %u\n", cinfo->image_width);
  fprintf(stderr, "[sandbox-child]   image_height     = %u\n", cinfo->image_height);
  fprintf(stderr, "[sandbox-child]   input_components = %d\n", cinfo->input_components);
  fprintf(stderr, "[sandbox-child]   next_scanline    = %u\n", cinfo->next_scanline);
  fprintf(stderr, "[sandbox-child]   err              = %p\n", (void*)cinfo->err);
  fprintf(stderr, "[sandbox-child]   dest             = %p\n", (void*)cinfo->dest);
  fprintf(stderr, "[sandbox-child]   sizeof struct    = %zu\n", sizeof(struct jpeg_compress_struct));
  // Hex dump first 64 bytes
  unsigned char *raw = (unsigned char *)cinfo;
  fprintf(stderr, "[sandbox-child]   hex[0..64):\n");
  for (int i = 0; i < 64; i += 16) {
    fprintf(stderr, "    +%04d: ", i);
    for (int j = 0; j < 16; j++) fprintf(stderr, "%02x ", raw[i+j]);
    fprintf(stderr, "\n");
  }
  fflush(stderr);
}

void jpeg_report_struct_layout(void)
{
  fprintf(stderr, "[sandbox-layout] system offsetof:\n");
  fprintf(stderr, "[sandbox-layout]   err               = %zu\n", offsetof(struct jpeg_compress_struct, err));
  fprintf(stderr, "[sandbox-layout]   mem               = %zu\n", offsetof(struct jpeg_compress_struct, mem));
  fprintf(stderr, "[sandbox-layout]   progress          = %zu\n", offsetof(struct jpeg_compress_struct, progress));
  fprintf(stderr, "[sandbox-layout]   client_data       = %zu\n", offsetof(struct jpeg_compress_struct, client_data));
  fprintf(stderr, "[sandbox-layout]   is_decompressor   = %zu\n", offsetof(struct jpeg_compress_struct, is_decompressor));
  fprintf(stderr, "[sandbox-layout]   global_state      = %zu\n", offsetof(struct jpeg_compress_struct, global_state));
  fprintf(stderr, "[sandbox-layout]   dest              = %zu\n", offsetof(struct jpeg_compress_struct, dest));
  fprintf(stderr, "[sandbox-layout]   image_width       = %zu\n", offsetof(struct jpeg_compress_struct, image_width));
  fprintf(stderr, "[sandbox-layout]   image_height      = %zu\n", offsetof(struct jpeg_compress_struct, image_height));
  fprintf(stderr, "[sandbox-layout]   input_components  = %zu\n", offsetof(struct jpeg_compress_struct, input_components));
  fprintf(stderr, "[sandbox-layout]   in_color_space    = %zu\n", offsetof(struct jpeg_compress_struct, in_color_space));
  fprintf(stderr, "[sandbox-layout]   input_gamma       = %zu\n", offsetof(struct jpeg_compress_struct, input_gamma));
  fprintf(stderr, "[sandbox-layout]   data_precision    = %zu\n", offsetof(struct jpeg_compress_struct, data_precision));
  fprintf(stderr, "[sandbox-layout]   num_components    = %zu\n", offsetof(struct jpeg_compress_struct, num_components));
  fprintf(stderr, "[sandbox-layout]   restart_interval  = %zu\n", offsetof(struct jpeg_compress_struct, restart_interval));
  fprintf(stderr, "[sandbox-layout]   next_scanline     = %zu\n", offsetof(struct jpeg_compress_struct, next_scanline));
  fprintf(stderr, "[sandbox-layout]   sizeof struct     = %zu\n", sizeof(struct jpeg_compress_struct));
  fprintf(stderr, "[sandbox-layout]   JPEG_LIB_VERSION  = %d\n", JPEG_LIB_VERSION);
  fflush(stderr);
}

int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;
    // Stay alive while the shim's RPC thread handles requests
    while (1) {
        pause();
    }
    return 0;
}

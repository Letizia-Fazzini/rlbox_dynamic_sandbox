#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <jpeglib.h>



int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;
    // Stay alive while the shim's RPC thread handles requests
    while (1) {
        pause();
    }
    return 0;
}

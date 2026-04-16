#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;
    // Stay alive while the shim's RPC thread handles requests
    while (1) {
        pause();
    }
    return 0;
}

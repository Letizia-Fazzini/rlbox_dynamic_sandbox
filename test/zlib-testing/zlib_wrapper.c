#include "zlib.h"

/* Wrapper functions to expose common zlib macros as callable functions */

/* deflateInit macro wrapper */
int deflateInitWrapper(z_streamp strm, int level) {
    return deflateInit_(strm, level, ZLIB_VERSION, (int)sizeof(z_stream));
}

/* inflateInit macro wrapper */
int inflateInitWrapper(z_streamp strm) {
    return inflateInit_(strm, ZLIB_VERSION, (int)sizeof(z_stream));
}

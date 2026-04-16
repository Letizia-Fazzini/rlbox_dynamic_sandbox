#ifndef RLBOX_TEST_GLUE_LIB_H
#define RLBOX_TEST_GLUE_LIB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Test library exposed to the process sandbox via dlsym(RTLD_DEFAULT, ...).
 *
 * All functions use int64_t for parameters and return value because the
 * shim's invoke handler calls functions via libffi with ffi_type_sint64
 * for every argument and for the return value.
 */

int64_t glue_noop(void);
int64_t glue_add(int64_t a, int64_t b);
int64_t glue_multiply(int64_t a, int64_t b);
int64_t glue_sum4(int64_t a, int64_t b, int64_t c, int64_t d);
int64_t glue_read_int64(int64_t* ptr);
int64_t glue_write_int64(int64_t* ptr, int64_t val);
int64_t glue_memset_pattern(int64_t* buf, int64_t count, int64_t pattern);

#ifdef __cplusplus
}
#endif

#endif

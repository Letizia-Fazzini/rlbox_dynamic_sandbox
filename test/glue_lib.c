#include "glue_lib.h"

int64_t glue_noop(void)
{
  return 42;
}

int64_t glue_add(int64_t a, int64_t b)
{
  return a + b;
}

int64_t glue_multiply(int64_t a, int64_t b)
{
  return a * b;
}

int64_t glue_sum4(int64_t a, int64_t b, int64_t c, int64_t d)
{
  return a + b + c + d;
}

int64_t glue_read_int64(int64_t* ptr)
{
  if (!ptr) {
    return 0;
  }
  return *ptr;
}

int64_t glue_write_int64(int64_t* ptr, int64_t val)
{
  if (!ptr) {
    return 0;
  }
  *ptr = val;
  return val;
}

int64_t glue_memset_pattern(int64_t* buf, int64_t count, int64_t pattern)
{
  if (!buf || count <= 0) {
    return 0;
  }
  for (int64_t i = 0; i < count; ++i) {
    buf[i] = pattern;
  }
  return count;
}

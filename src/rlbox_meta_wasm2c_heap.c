// `--wrap` overrides for the wasm2c memory allocators.  Default path
// delegates to upstream; meta-sandbox injection points wasm's linear
// memory at a shared memfd it manages.
//
// Two entry points need wrapping:
//   wasm_rt_allocate_memory   -- called when the wasm module exports its
//                                own memory (wasi-sdk default; zlib).
//   create_wasm2c_memory      -- called when the wasm module imports its
//                                memory from the host (rlbox imported-mem
//                                builds; libjpeg under some configs).
// Both are intercepted; whichever fires uses the same g_pending heap.

#include "rlbox_meta_wasm2c_heap.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WASM_PAGE_SIZE 65536u

// Mirror wasm2c's wasm_rt_memory_t layout from
// _deps/wasm2c_compiler-src/wasm2c/wasm-rt.h.  data_end is unused on
// little-endian targets (MEM_ADDR uses data + addr) so leaving it
// initialised on borrowed heap is safe.  is64 is bool (1 byte + pad).
typedef struct
{
  uint8_t* data;
  uint8_t* data_end;
  uint32_t page_size;
  uint64_t pages;
  uint64_t max_pages;
  uint64_t size;
  bool is64;
} wasm_rt_memory_t;

typedef struct
{
  int is_valid;
  int is_mem_32;
  uint32_t max_pages;
  uint64_t max_size;
} w2c_mem_capacity;

extern wasm_rt_memory_t __real_create_wasm2c_memory(
  uint32_t initial_pages,
  const w2c_mem_capacity* custom_capacity,
  const char* name);
extern void __real_destroy_wasm2c_memory(wasm_rt_memory_t* memory);
extern void __real_wasm_rt_allocate_memory(wasm_rt_memory_t* memory,
                                           uint64_t initial_pages,
                                           uint64_t max_pages,
                                           bool is64,
                                           uint32_t page_size);
extern void __real_wasm_rt_free_memory(wasm_rt_memory_t* memory);

static struct
{
  void* base;
  uint32_t initial_pages;
  uint32_t max_pages;
  int armed;
} g_pending = { 0 };

#define BORROWED_CAP 8
static void* g_borrowed[BORROWED_CAP];
static pthread_mutex_t g_borrowed_mu = PTHREAD_MUTEX_INITIALIZER;

void rlbox_meta_set_pending_shared_heap(void* base,
                                        uint32_t initial_pages,
                                        uint32_t max_pages)
{
  g_pending.base = base;
  g_pending.initial_pages = initial_pages;
  g_pending.max_pages = max_pages;
  g_pending.armed = (base != NULL);
}

void rlbox_meta_record_borrowed_heap(void* data)
{
  if (!data) {
    return;
  }
  pthread_mutex_lock(&g_borrowed_mu);
  for (int i = 0; i < BORROWED_CAP; ++i) {
    if (g_borrowed[i] == NULL) {
      g_borrowed[i] = data;
      break;
    }
  }
  pthread_mutex_unlock(&g_borrowed_mu);
}

void rlbox_meta_forget_borrowed_heap(void* data)
{
  if (!data) {
    return;
  }
  pthread_mutex_lock(&g_borrowed_mu);
  for (int i = 0; i < BORROWED_CAP; ++i) {
    if (g_borrowed[i] == data) {
      g_borrowed[i] = NULL;
      break;
    }
  }
  pthread_mutex_unlock(&g_borrowed_mu);
}

int rlbox_meta_is_borrowed_heap(const void* data)
{
  if (!data) {
    return 0;
  }
  int found = 0;
  pthread_mutex_lock(&g_borrowed_mu);
  for (int i = 0; i < BORROWED_CAP; ++i) {
    if (g_borrowed[i] == data) {
      found = 1;
      break;
    }
  }
  pthread_mutex_unlock(&g_borrowed_mu);
  return found;
}

// Exported-memory wasm modules (wasi-sdk default) call this.  We honor
// the module's `initial_pages` for `pages` (so wasi-sdk's internal sbrk
// pointer lines up with the wasm-side static `__heap_base`/`__heap_end`
// link symbols) and cap `max_pages` at the meta's wasm-reserved partition
// so `memory.grow` cannot extend into the process mspace's range.
void __wrap_wasm_rt_allocate_memory(wasm_rt_memory_t* memory,
                                    uint64_t initial_pages,
                                    uint64_t max_pages,
                                    bool is64,
                                    uint32_t page_size)
{
  if (g_pending.armed && g_pending.base != NULL) {
    memset(memory, 0, sizeof(*memory));
    memory->data = (uint8_t*)g_pending.base;
    memory->size = (uint64_t)initial_pages * page_size;
    memory->data_end = memory->data + memory->size;
    memory->pages = initial_pages;
    memory->max_pages =
      (g_pending.max_pages > max_pages) ? max_pages : g_pending.max_pages;
    memory->page_size = page_size;
    memory->is64 = is64;
    rlbox_meta_record_borrowed_heap(memory->data);
    g_pending.armed = 0;
    g_pending.base = NULL;
    return;
  }
  __real_wasm_rt_allocate_memory(
    memory, initial_pages, max_pages, is64, page_size);
}

void __wrap_wasm_rt_free_memory(wasm_rt_memory_t* memory)
{
  if (!memory || memory->data == NULL) {
    return;
  }
  if (rlbox_meta_is_borrowed_heap(memory->data)) {
    rlbox_meta_forget_borrowed_heap(memory->data);
    memory->data = NULL;
    return;
  }
  __real_wasm_rt_free_memory(memory);
}

// Imported-memory variant.  rlbox_wasm2c_sandbox.hpp's bring-up calls
// this when the module imports its memory from the host.  Same shape as
// the exported variant: honor module-supplied initial_pages, cap
// max_pages at the meta's wasm-reserved partition.
wasm_rt_memory_t __wrap_create_wasm2c_memory(
  uint32_t initial_pages,
  const w2c_mem_capacity* custom_capacity,
  const char* name)
{
  if (g_pending.armed && g_pending.base != NULL) {
    wasm_rt_memory_t ret;
    memset(&ret, 0, sizeof(ret));
    ret.data = (uint8_t*)g_pending.base;
    ret.size = (uint64_t)initial_pages * WASM_PAGE_SIZE;
    ret.data_end = ret.data + ret.size;
    ret.pages = initial_pages;
    ret.max_pages = g_pending.max_pages;
    ret.page_size = WASM_PAGE_SIZE;
    ret.is64 = false;
    rlbox_meta_record_borrowed_heap(ret.data);
    g_pending.armed = 0;
    g_pending.base = NULL;
    (void)custom_capacity;
    (void)name;
    return ret;
  }
  return __real_create_wasm2c_memory(initial_pages, custom_capacity, name);
}

void __wrap_destroy_wasm2c_memory(wasm_rt_memory_t* memory)
{
  if (!memory || memory->data == NULL) {
    return;
  }
  if (rlbox_meta_is_borrowed_heap(memory->data)) {
    rlbox_meta_forget_borrowed_heap(memory->data);
    memory->data = NULL;
    return;
  }
  __real_destroy_wasm2c_memory(memory);
}

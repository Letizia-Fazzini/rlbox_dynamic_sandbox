// Hooks the meta sandbox uses to inject a shared memfd as wasm2c's linear
// memory.  When `rlbox_meta_set_pending_shared_heap` is called before
// `wasm_sbx.impl_create_sandbox()`, the next `create_wasm2c_memory` call
// returns a wasm_rt_memory_t pointing at the supplied buffer instead of
// allocating one.  Implemented as `--wrap` overrides on
// `create_wasm2c_memory` / `destroy_wasm2c_memory` so the wasm2c source
// stays unmodified.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void rlbox_meta_set_pending_shared_heap(void* base,
                                        uint32_t initial_pages,
                                        uint32_t max_pages);

// Returns 1 if the wasm rt memory at `data` was injected by the meta
// (so destroy must skip the unmap), 0 otherwise.
int rlbox_meta_is_borrowed_heap(const void* data);

// Records a borrowed-heap pointer so destroy can recognize it.
void rlbox_meta_record_borrowed_heap(void* data);

// Removes a borrowed-heap pointer from the registry.
void rlbox_meta_forget_borrowed_heap(void* data);

#ifdef __cplusplus
}
#endif

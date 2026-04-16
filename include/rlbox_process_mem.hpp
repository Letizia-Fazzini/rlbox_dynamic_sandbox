#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/mman.h>
#include <unistd.h>

namespace rlbox {

inline void* os_mmap_aligned(size_t requested_length, size_t alignment)
{
  size_t padded_length = requested_length + alignment;
  void* unaligned_ptr =
    mmap(NULL, padded_length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (unaligned_ptr == MAP_FAILED) {
    return nullptr;
  }

  uintptr_t unaligned_addr = reinterpret_cast<uintptr_t>(unaligned_ptr);
  uintptr_t aligned_addr =
    (unaligned_addr + (alignment - 1)) & ~(alignment - 1);

  size_t unused_front = aligned_addr - unaligned_addr;
  if (unused_front > 0) {
    munmap(unaligned_ptr, unused_front);
  }

  size_t unused_back = padded_length - unused_front - requested_length;
  if (unused_back > 0) {
    munmap(reinterpret_cast<void*>(aligned_addr + requested_length),
           unused_back);
  }

  return reinterpret_cast<void*>(aligned_addr);
}

}

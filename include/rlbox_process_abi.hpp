#pragma once

#include <cstdint>
#include <tuple>
#include <type_traits>

// Width of the *shim*'s long/pointer types in bits.  Set to 32 when the
// shim is built as i386 (Phase 7c); the host build is told via cmake.  The
// shim sees its own sizeof(long), which by construction agrees with this.
#ifndef RLBOX_PROCESS_SHIM_TARGET_BITS
#define RLBOX_PROCESS_SHIM_TARGET_BITS 64
#endif

// Shared ABI header for host and child shim. Every arg + return is
// widened to an int64 wire slot; a parallel arg_type tag vector tells
// the shim how to cast each slot when building the libffi cif.

// Layout of the shared memfd, agreed between host, shim, and the
// wasm2c runtime when the meta sandbox injects this region as wasm's
// linear memory.  The wasm allocator carves from offset 0 upward;
// the process mspace anchors at RLBOX_SHM_PROCESS_OFFSET and grows
// upward.  Both allocators share the same memfd but live in disjoint
// offset ranges so they cannot collide.
#define RLBOX_SHM_REGION_BYTES   ((size_t)1 << 30)        // 1 GiB
#define RLBOX_SHM_PROCESS_OFFSET ((size_t)768 << 20)      // 768 MiB
#define RLBOX_SHM_PROCESS_BYTES  (RLBOX_SHM_REGION_BYTES - RLBOX_SHM_PROCESS_OFFSET)

namespace rlbox {

enum arg_type : int32_t
{
  ARG_VOID = 0,
  ARG_SINT32 = 1,
  ARG_UINT32 = 2,
  ARG_SINT64 = 3,
  ARG_UINT64 = 4,
  // Data pointer: 32-bit shared-region offset on the wire; both sides
  // recover the absolute VA by adding their local shared-region base.
  ARG_POINTER = 5,
  // Function pointer (callback trampoline): shim-side absolute address
  // on the wire.  Lives in the shim's text segment, not in the shared
  // region, so it can't be encoded as an offset.
  ARG_CALLBACK_HANDLE = 6,
};

namespace abi_detail {

template<typename T>
struct function_traits;

template<typename R, typename... Args>
struct function_traits<R(Args...)>
{
  using return_type = R;
  using args_tuple = std::tuple<Args...>;
};

// Map an original (pre-rlbox-conversion) C++ type to a wire tag. Keyed
// on the original type so a real pointer is distinguishable from a
// uint64_t the same width as T_PointerType.
template<typename T>
struct tag_of;

template<> struct tag_of<void>               { static constexpr arg_type value = ARG_VOID;   };
template<> struct tag_of<bool>               { static constexpr arg_type value = ARG_SINT32; };
template<> struct tag_of<char>               { static constexpr arg_type value = ARG_SINT32; };
template<> struct tag_of<signed char>        { static constexpr arg_type value = ARG_SINT32; };
template<> struct tag_of<unsigned char>      { static constexpr arg_type value = ARG_UINT32; };
template<> struct tag_of<short>              { static constexpr arg_type value = ARG_SINT32; };
template<> struct tag_of<unsigned short>     { static constexpr arg_type value = ARG_UINT32; };
template<> struct tag_of<int>                { static constexpr arg_type value = ARG_SINT32; };
template<> struct tag_of<unsigned int>       { static constexpr arg_type value = ARG_UINT32; };
// Width of `long` is decided by the *shim's* arch, not the host's.  When the
// host is x86_64 but the shim is i386, the host must encode `long` as 32 bits
// to match what the i386 callee actually takes.
template<> struct tag_of<long>
{
  static constexpr arg_type value =
    (RLBOX_PROCESS_SHIM_TARGET_BITS == 64) ? ARG_SINT64 : ARG_SINT32;
};
template<> struct tag_of<unsigned long>
{
  static constexpr arg_type value =
    (RLBOX_PROCESS_SHIM_TARGET_BITS == 64) ? ARG_UINT64 : ARG_UINT32;
};
template<> struct tag_of<long long>          { static constexpr arg_type value = ARG_SINT64; };
template<> struct tag_of<unsigned long long> { static constexpr arg_type value = ARG_UINT64; };

template<typename T>
struct tag_of<T*> { static constexpr arg_type value = ARG_POINTER; };

// Function-pointer specialization is more specific than T* so it wins
// partial-ordering: a registered callback rides ARG_CALLBACK_HANDLE
// (absolute trampoline VA) instead of ARG_POINTER (shared-region offset).
template<typename R, typename... A>
struct tag_of<R(*)(A...)>
{
  static constexpr arg_type value = ARG_CALLBACK_HANDLE;
};

template<typename T>
struct tag_of<const T> { static constexpr arg_type value = tag_of<T>::value; };

template<typename T>
constexpr arg_type tag_of_v = tag_of<std::remove_cv_t<std::remove_reference_t<T>>>::value;

} // namespace abi_detail

} // namespace rlbox

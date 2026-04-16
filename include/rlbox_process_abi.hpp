#pragma once

#include <cstdint>
#include <tuple>
#include <type_traits>

// Shared ABI header included by the host (rlbox_process_sandbox.hpp) and
// the child shim (rlbox_process_sandbox_shim.cpp).
//
// Every argument and the return value is widened to an int64_t wire slot.
// A parallel vector of arg_type tags tells the shim how to cast each slot
// when building the libffi cif and whether to translate sandbox offsets
// into child-absolute addresses.

namespace rlbox {

enum arg_type : int32_t
{
  ARG_VOID = 0,
  ARG_SINT32 = 1,
  ARG_UINT32 = 2,
  ARG_SINT64 = 3,
  ARG_UINT64 = 4,
  // POINTER-tagged slots carry a sandbox offset on the wire.  The shim
  // adds g_shm_base to recover the child-absolute address before ffi_call;
  // the host already emits offsets because rlbox converts tainted<T*> to
  // T_PointerType (uintptr_t offset) before handing it to us.
  ARG_POINTER = 5,
};

namespace abi_detail {

// Decompose a function type `R(Args...)` into its parts.  Only used on the
// host side; the shim sees the tags on the wire.
template<typename T>
struct function_traits;

template<typename R, typename... Args>
struct function_traits<R(Args...)>
{
  using return_type = R;
  using args_tuple = std::tuple<Args...>;
};

// Map an *original* C++ type (pre-rlbox conversion) to a wire tag.  We
// key on the original type so we can distinguish a real pointer from a
// uint64_t that happens to share a width with our T_PointerType.
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
template<> struct tag_of<long>
{
  static constexpr arg_type value = (sizeof(long) == 8) ? ARG_SINT64 : ARG_SINT32;
};
template<> struct tag_of<unsigned long>
{
  static constexpr arg_type value = (sizeof(long) == 8) ? ARG_UINT64 : ARG_UINT32;
};
template<> struct tag_of<long long>          { static constexpr arg_type value = ARG_SINT64; };
template<> struct tag_of<unsigned long long> { static constexpr arg_type value = ARG_UINT64; };

template<typename T>
struct tag_of<T*> { static constexpr arg_type value = ARG_POINTER; };

template<typename T>
struct tag_of<const T> { static constexpr arg_type value = tag_of<T>::value; };

template<typename T>
constexpr arg_type tag_of_v = tag_of<std::remove_cv_t<std::remove_reference_t<T>>>::value;

} // namespace abi_detail

} // namespace rlbox

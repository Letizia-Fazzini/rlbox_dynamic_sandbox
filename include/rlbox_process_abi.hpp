#pragma once

#include <cstdint>
#include <tuple>
#include <type_traits>

// Shared ABI header for host and child shim. Every arg + return is
// widened to an int64 wire slot; a parallel arg_type tag vector tells
// the shim how to cast each slot when building the libffi cif.

namespace rlbox {

enum arg_type : int32_t
{
  ARG_VOID = 0,
  ARG_SINT32 = 1,
  ARG_UINT32 = 2,
  ARG_SINT64 = 3,
  ARG_UINT64 = 4,
  // Sandbox offset on the wire; shim recovers absolute address.
  ARG_POINTER = 5,
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

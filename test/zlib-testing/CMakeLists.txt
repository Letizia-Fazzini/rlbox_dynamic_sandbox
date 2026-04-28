cmake_minimum_required(VERSION 3.14)

# TODO: Migrate from FetchContent_Populate
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()

project(rlbox_wasm_example
	VERSION 0.1
	DESCRIPTION "RLBox rlbox_wasm_example")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optional whole-project i386 build (matches the top-level RLBOX_BUILD_M32
# switch).  Off-path is the staging ground for Config B (host x86_64, shim
# i386) -- the eventual CMake split will replace this with per-target flags.
option(RLBOX_BUILD_M32 "Force -m32 on every native target" OFF)
if(RLBOX_BUILD_M32)
  add_compile_options(-m32)
  add_link_options(-m32)
  message(STATUS "zlib-testing: -m32 forced on every target")
endif()

find_package(ZLIB REQUIRED)

include(FetchContent)

FetchContent_Declare(rlbox
  GIT_REPOSITORY https://github.com/PLSysSec/rlbox.git)
FetchContent_GetProperties(rlbox)
if(NOT rlbox_POPULATED)
  FetchContent_Populate(rlbox)
endif()

FetchContent_Declare(rlbox_wasm2c_sandbox
  GIT_REPOSITORY https://github.com/PLSysSec/rlbox_wasm2c_sandbox.git)
FetchContent_GetProperties(rlbox_wasm2c_sandbox)
if(NOT rlbox_wasm2c_sandbox_POPULATED)
  FetchContent_Populate(rlbox_wasm2c_sandbox)
endif()

string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" CMAKE_HOST_SYSTEM_PROCESSOR_LOWER)

if (${CMAKE_HOST_SYSTEM_PROCESSOR_LOWER} MATCHES "x86_64" OR ${CMAKE_HOST_SYSTEM_PROCESSOR_LOWER} MATCHES "amd64")
  if (WIN32)
    set (wasi-clang-url "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-27/wasi-sdk-27.0-x86_64-windows.tar.gz")
  elseif (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set (wasi-clang-url "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-27/wasi-sdk-27.0-x86_64-macos.tar.gz")
  elseif (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    set (wasi-clang-url "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-27/wasi-sdk-27.0-x86_64-linux.tar.gz")
  else()
    message (ERROR "Unknown OS")
  endif()
elseif (${CMAKE_HOST_SYSTEM_PROCESSOR_LOWER} MATCHES "arm64")
  if (WIN32)
    set (wasi-clang-url "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-27/wasi-sdk-27.0-arm64-windows.tar.gz")
  elseif (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set (wasi-clang-url "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-27/wasi-sdk-27.0-arm64-macos.tar.gz")
  elseif (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    set (wasi-clang-url "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-27/wasi-sdk-27.0-arm64-linux.tar.gz")
  else()
    message (ERROR "Unknown OS")
  endif()
else()
  message (ERROR "Unknown arch")
endif()

FetchContent_Declare(wasiclang
  URL ${wasi-clang-url}
  DOWNLOAD_EXTRACT_TIMESTAMP false
)
FetchContent_GetProperties(wasiclang)
if(NOT wasiclang_POPULATED)
  FetchContent_Populate(wasiclang)
endif()

FetchContent_Declare(
  wasm2c_compiler
  GIT_REPOSITORY https://github.com/WebAssembly/wabt/
  GIT_TAG main)
FetchContent_GetProperties(wasm2c_compiler)
if(NOT wasm2c_compiler_POPULATED)
  FetchContent_Populate(wasm2c_compiler)
endif()

################### Build wasm2c ###################

set(WASM2C_RUNTIME_DIR "${wasm2c_compiler_SOURCE_DIR}/build/")

if(MSVC)
  set(WASM2C_PATH ${WASM2C_RUNTIME_DIR}/${CMAKE_BUILD_TYPE}/wasm2c.exe)
else()
  set(WASM2C_PATH ${WASM2C_RUNTIME_DIR}/wasm2c)
endif()

add_custom_command(OUTPUT "${WASM2C_PATH}"
                   WORKING_DIRECTORY "${wasm2c_compiler_SOURCE_DIR}"
                   COMMAND ${CMAKE_COMMAND}
                           -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
                           -S  .
                           -B ${WASM2C_RUNTIME_DIR}
                   COMMAND ${CMAKE_COMMAND}
                           --build ${WASM2C_RUNTIME_DIR}
                           --config ${CMAKE_BUILD_TYPE}
                           --parallel
                   COMMENT "Building wasm2c compiler and runtime")

################### Fetch and Build zlib for WASM ###################

# Fetch zlib source
FetchContent_Declare(zlib_wasm
  GIT_REPOSITORY https://github.com/madler/zlib.git
  GIT_TAG v1.2.11
)
FetchContent_GetProperties(zlib_wasm)
if(NOT zlib_wasm_POPULATED)
  FetchContent_Populate(zlib_wasm)
endif()

# Compile zlib to WASM
set(ZLIB_WASM "${CMAKE_BINARY_DIR}/zlib.wasm")
set(ZLIB_H "${CMAKE_BINARY_DIR}/zlib.wasm.h")
set(ZLIB_C "${CMAKE_BINARY_DIR}/zlib.wasm.c")

# List of zlib source files to compile (core files only; exclude gz* for WASI compatibility)
set(ZLIB_SOURCES
  ${zlib_wasm_SOURCE_DIR}/adler32.c
  ${zlib_wasm_SOURCE_DIR}/compress.c
  ${zlib_wasm_SOURCE_DIR}/crc32.c
  ${zlib_wasm_SOURCE_DIR}/deflate.c
  ${zlib_wasm_SOURCE_DIR}/infback.c
  ${zlib_wasm_SOURCE_DIR}/inflate.c
  ${zlib_wasm_SOURCE_DIR}/inftrees.c
  ${zlib_wasm_SOURCE_DIR}/inffast.c
  ${zlib_wasm_SOURCE_DIR}/trees.c
  ${zlib_wasm_SOURCE_DIR}/uncompr.c
  ${zlib_wasm_SOURCE_DIR}/zutil.c
  ${CMAKE_SOURCE_DIR}/zlib_wrapper.c
)

add_custom_command(OUTPUT "${ZLIB_H}" "${ZLIB_C}" "${ZLIB_WASM}"
                   DEPENDS ${ZLIB_SOURCES} ${C_DUMMY_MAIN} ${WASM2C_PATH}
                   COMMAND ${wasiclang_SOURCE_DIR}/bin/clang
                           --sysroot ${wasiclang_SOURCE_DIR}/share/wasi-sysroot/
                           -O3
                           -Wl,--export-all -Wl,--no-entry -Wl,--growable-table -Wl,--stack-first -Wl,-z,stack-size=1048576 -Wl,--export-table
                           -I${zlib_wasm_SOURCE_DIR}  # Include zlib headers
                           -o ${ZLIB_WASM}
                           ${C_DUMMY_MAIN}
                           ${ZLIB_SOURCES}
                   COMMAND ${WASM2C_PATH}
                           -o ${ZLIB_C}
                           ${ZLIB_WASM}
                   COMMENT "Building zlib as WASM sandboxed library")

################### Build the wasm sandboxed native library ###################

set(WASM2C_RUNTIME_CODE ${wasm2c_compiler_SOURCE_DIR}/wasm2c/wasm-rt-impl.c
                        ${wasm2c_compiler_SOURCE_DIR}/wasm2c/wasm-rt-mem-impl.c
                        ${rlbox_wasm2c_sandbox_SOURCE_DIR}/src/wasm2c_rt_minwasi.c
                        ${rlbox_wasm2c_sandbox_SOURCE_DIR}/src/wasm2c_rt_mem.c)

add_library(zlib_sandboxed STATIC ${ZLIB_C} ${WASM2C_RUNTIME_CODE})
target_include_directories(zlib_sandboxed PUBLIC ${wasm2c_compiler_SOURCE_DIR}/wasm2c
                                 PUBLIC ${wasm2c_compiler_SOURCE_DIR}/third_party/simde
                                 PUBLIC ${rlbox_wasm2c_sandbox_SOURCE_DIR}/include
                                 PUBLIC ${CMAKE_BINARY_DIR}
                                 PUBLIC ${zlib_wasm_SOURCE_DIR})

target_compile_definitions(zlib_sandboxed PUBLIC WASM_RT_USE_MMAP=1
                                 PUBLIC WASM_RT_SKIP_SIGNAL_RECOVERY=1
                                 PUBLIC WASM_RT_NONCONFORMING_UNCHECKED_STACK_EXHAUSTION=1)


add_executable(main main.cpp)
target_include_directories(main PUBLIC ${rlbox_SOURCE_DIR}/code/include
                                PUBLIC ${rlbox_wasm2c_sandbox_SOURCE_DIR}/include
                                PUBLIC ${zlib_wasm_SOURCE_DIR})
target_link_libraries(main zlib_sandboxed pthread ZLIB::ZLIB)

# One-shot "stress" variant: one deflate(Z_FINISH) invoke over the whole
# stream. Amortizes per-call overhead -- the companion to main_process_stress
# so process fork() cost is paid once per compression.
add_executable(main_stress main_oneshot.cpp)
target_include_directories(main_stress PUBLIC ${rlbox_SOURCE_DIR}/code/include
                                       PUBLIC ${rlbox_wasm2c_sandbox_SOURCE_DIR}/include
                                       PUBLIC ${zlib_wasm_SOURCE_DIR})
target_link_libraries(main_stress zlib_sandboxed pthread ZLIB::ZLIB)

################### Process-sandbox variant ###################
#
# Builds a second binary that runs zlib via rlbox_process_sandbox (fork +
# LD_PRELOAD + RPC) instead of wasm2c.  Pulls shim/build infrastructure
# from the parent rlbox_dynamic_sandbox repo so we don't duplicate it.

set(RLBOX_DYNAMIC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(RLBOX_DYNAMIC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${RLBOX_DYNAMIC_ROOT} rlbox_dynamic_parent EXCLUDE_FROM_ALL)

# Child-side wrapper: a long-running process whose dynamic symbol table
# exposes zlib to the shim's dlsym(RTLD_DEFAULT, ...).  -rdynamic keeps
# the linker from pruning zlib's externally-referenced symbols, and the
# zlib_wrapper.c file contributes deflateInitWrapper / inflateInitWrapper
# so we can call the deflateInit_ macro from the host side.
#
# Under RLBOX_PROCESS_SHIM_ARCH=i386 the wrapper is built i386 by the
# parent ExternalProject (target zlib_process_wrapper_i386, IMPORTED).
if(NOT RLBOX_PROCESS_SHIM_ARCH STREQUAL "i386")
  add_executable(zlib_process_wrapper
                 ${RLBOX_DYNAMIC_ROOT}/c_src/process_sandbox_wrapper.c
                 ${CMAKE_CURRENT_SOURCE_DIR}/zlib_wrapper.c)
  target_link_libraries(zlib_process_wrapper PRIVATE ZLIB::ZLIB)
  target_link_options(zlib_process_wrapper PRIVATE "-rdynamic")
endif()

# Wrapper path + shim file expressions used by every host-side consumer
# below.  Switch them to the imported i386 targets when configured.
if(RLBOX_PROCESS_SHIM_ARCH STREQUAL "i386")
  set(_rlbox_zlib_wrapper_target zlib_process_wrapper_i386)
  set(_rlbox_zlib_wrapper_path $<TARGET_FILE:zlib_process_wrapper_i386>)
  set(_rlbox_shim_target sandbox_shim_i386)
  set(_rlbox_shim_path $<TARGET_FILE:sandbox_shim_i386>)
else()
  set(_rlbox_zlib_wrapper_target zlib_process_wrapper)
  set(_rlbox_zlib_wrapper_path $<TARGET_FILE:zlib_process_wrapper>)
  set(_rlbox_shim_target sandbox_shim)
  set(_rlbox_shim_path $<TARGET_FILE:sandbox_shim>)
endif()

# Host-side main that uses the process sandbox.  The sandbox hard-codes
# LD_PRELOAD="./sandbox_shim.so" relative to CWD, so we copy the shim
# next to this binary.
add_executable(main_process
               main_process.cpp
               ${RLBOX_DYNAMIC_ROOT}/src/rlbox_process_tls.cpp)
target_include_directories(main_process PRIVATE
                           ${rlbox_SOURCE_DIR}/code/include
                           ${RLBOX_DYNAMIC_ROOT}/include)
target_link_libraries(main_process PRIVATE
                      rlbox_dynamic
                      pthread
                      ZLIB::ZLIB)
target_compile_definitions(main_process PRIVATE
  ZLIB_PROCESS_WRAPPER_PATH="${_rlbox_zlib_wrapper_path}")

add_custom_command(TARGET main_process POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E copy_if_different
                           ${_rlbox_shim_path}
                           $<TARGET_FILE_DIR:main_process>/sandbox_shim.so
                   DEPENDS ${_rlbox_shim_target})
add_dependencies(main_process ${_rlbox_shim_target} ${_rlbox_zlib_wrapper_target})


add_executable(bench_native ${CMAKE_CURRENT_SOURCE_DIR}/bench_native.cpp)

# Process-backed "stress" variant -- one deflate invoke per stream.
add_executable(main_process_stress
               main_process_oneshot.cpp
               ${RLBOX_DYNAMIC_ROOT}/src/rlbox_process_tls.cpp)
target_include_directories(main_process_stress PRIVATE
                           ${rlbox_SOURCE_DIR}/code/include
                           ${RLBOX_DYNAMIC_ROOT}/include)
target_link_libraries(main_process_stress PRIVATE
                      rlbox_dynamic
                      pthread
                      ZLIB::ZLIB)
target_compile_definitions(main_process_stress PRIVATE
  ZLIB_PROCESS_WRAPPER_PATH="${_rlbox_zlib_wrapper_path}")

add_custom_command(TARGET main_process_stress POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E copy_if_different
                           ${_rlbox_shim_path}
                           $<TARGET_FILE_DIR:main_process_stress>/sandbox_shim.so
                   DEPENDS ${_rlbox_shim_target})
add_dependencies(main_process_stress ${_rlbox_shim_target} ${_rlbox_zlib_wrapper_target})

target_link_libraries(bench_native pthread ZLIB::ZLIB)

################### Meta-sandbox variant ###################
#
# Builds main_meta, which composes rlbox_process_sandbox + rlbox_wasm2c_sandbox
# via include/rlbox_meta_sandbox.hpp.  Runs under a user-selected
# policy: process, wasm, or adaptive.

add_executable(main_meta
               main_adaptive.cpp
               ${RLBOX_DYNAMIC_ROOT}/src/rlbox_process_tls.cpp
               ${RLBOX_DYNAMIC_ROOT}/src/rlbox_meta_wasm2c_heap.c)
target_include_directories(main_meta PRIVATE
                           ${rlbox_SOURCE_DIR}/code/include
                           ${rlbox_wasm2c_sandbox_SOURCE_DIR}/include
                           ${RLBOX_DYNAMIC_ROOT}/include
                           ${zlib_wasm_SOURCE_DIR}
                           ${CMAKE_BINARY_DIR})
# -rdynamic: wasm2c thunks must be in the dynamic symtab for meta's dlsym.
# --wrap=wasm_rt_allocate_memory et al: lets the meta inject the process
# backend's shared memfd as wasm's linear memory; the wraps delegate to
# upstream when no heap is pending, so non-shared paths are byte-identical.
target_link_options(main_meta PRIVATE
                    "-rdynamic"
                    "LINKER:--wrap=create_wasm2c_memory"
                    "LINKER:--wrap=destroy_wasm2c_memory"
                    "LINKER:--wrap=wasm_rt_allocate_memory"
                    "LINKER:--wrap=wasm_rt_free_memory")
target_link_libraries(main_meta PRIVATE
                      rlbox_dynamic
                      zlib_sandboxed
                      pthread
                      ZLIB::ZLIB)
target_compile_definitions(main_meta PRIVATE
  ZLIB_PROCESS_WRAPPER_PATH="${_rlbox_zlib_wrapper_path}")

add_custom_command(TARGET main_meta POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E copy_if_different
                           ${_rlbox_shim_path}
                           $<TARGET_FILE_DIR:main_meta>/sandbox_shim.so
                   DEPENDS ${_rlbox_shim_target})
add_dependencies(main_meta ${_rlbox_shim_target} ${_rlbox_zlib_wrapper_target})

# Cross-backend pointer integration test: mints a buffer through one
# backend's allocator and checksums it via the other backend's invoke path.
add_executable(test_cross_pointer
               test_cross_pointer.cpp
               ${RLBOX_DYNAMIC_ROOT}/src/rlbox_process_tls.cpp
               ${RLBOX_DYNAMIC_ROOT}/src/rlbox_meta_wasm2c_heap.c)
target_include_directories(test_cross_pointer PRIVATE
                           ${rlbox_SOURCE_DIR}/code/include
                           ${rlbox_wasm2c_sandbox_SOURCE_DIR}/include
                           ${RLBOX_DYNAMIC_ROOT}/include
                           ${zlib_wasm_SOURCE_DIR}
                           ${CMAKE_BINARY_DIR})
target_link_options(test_cross_pointer PRIVATE
                    "-rdynamic"
                    "LINKER:--wrap=create_wasm2c_memory"
                    "LINKER:--wrap=destroy_wasm2c_memory"
                    "LINKER:--wrap=wasm_rt_allocate_memory"
                    "LINKER:--wrap=wasm_rt_free_memory")
target_link_libraries(test_cross_pointer PRIVATE
                      rlbox_dynamic
                      zlib_sandboxed
                      pthread
                      ZLIB::ZLIB)
target_compile_definitions(test_cross_pointer PRIVATE
  ZLIB_PROCESS_WRAPPER_PATH="${_rlbox_zlib_wrapper_path}")

add_custom_command(TARGET test_cross_pointer POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E copy_if_different
                           ${_rlbox_shim_path}
                           $<TARGET_FILE_DIR:test_cross_pointer>/sandbox_shim.so
                   DEPENDS ${_rlbox_shim_target})
add_dependencies(test_cross_pointer ${_rlbox_shim_target} ${_rlbox_zlib_wrapper_target})

# Pure-policy unit tests for make_adaptive_*_policy.  Includes the meta
# header but never instantiates a sandbox -- runs synthetic
# meta_policy_context values through the policy closures directly.  Skips
# the heap-injection wrap and process-tls TUs that only matter when an
# actual sandbox is brought up.
add_executable(test_meta_policy test_meta_policy.cpp)
target_include_directories(test_meta_policy PRIVATE
                           ${rlbox_SOURCE_DIR}/code/include
                           ${rlbox_wasm2c_sandbox_SOURCE_DIR}/include
                           ${RLBOX_DYNAMIC_ROOT}/include
                           ${zlib_wasm_SOURCE_DIR}
                           ${CMAKE_BINARY_DIR})
target_link_libraries(test_meta_policy PRIVATE
                      rlbox_dynamic
                      zlib_sandboxed
                      pthread)

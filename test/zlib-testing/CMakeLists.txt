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
add_executable(zlib_process_wrapper
               ${RLBOX_DYNAMIC_ROOT}/c_src/process_sandbox_wrapper.c
               ${CMAKE_CURRENT_SOURCE_DIR}/zlib_wrapper.c)
target_link_libraries(zlib_process_wrapper PRIVATE ZLIB::ZLIB)
target_link_options(zlib_process_wrapper PRIVATE "-rdynamic")

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
  ZLIB_PROCESS_WRAPPER_PATH="$<TARGET_FILE:zlib_process_wrapper>")

add_custom_command(TARGET main_process POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E copy_if_different
                           $<TARGET_FILE:sandbox_shim>
                           $<TARGET_FILE_DIR:main_process>/sandbox_shim.so
                   DEPENDS sandbox_shim)
add_dependencies(main_process sandbox_shim zlib_process_wrapper)

add_executable(bench_native ${CMAKE_CURRENT_SOURCE_DIR}/bench_native.c)
target_link_libraries(bench_native pthread ZLIB::ZLIB)

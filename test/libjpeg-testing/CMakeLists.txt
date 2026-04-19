cmake_minimum_required(VERSION 3.30)

# TODO: Migrate from FetchContent_Populate
cmake_policy(SET CMP0169 OLD)

project(rlbox_wasm_example
	VERSION 0.1
	DESCRIPTION "RLBox rlbox_wasm_example")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(JPEG REQUIRED)

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

################### Fetch and Build libjpeg-turbo for WASM ###################

# Fetch libjpeg-turbo source
FetchContent_Declare(jpeg_wasm
  GIT_REPOSITORY https://github.com/libjpeg-turbo/libjpeg-turbo.git
  GIT_TAG 3.0.1
)
FetchContent_GetProperties(jpeg_wasm)
if(NOT jpeg_wasm_POPULATED)
  FetchContent_Populate(jpeg_wasm)
endif()

# Generate config headers required by libjpeg-turbo for the WASM build
set(JPEG_WASM_CONFIG_DIR "${CMAKE_BINARY_DIR}/jpeg_wasm_config")
file(MAKE_DIRECTORY ${JPEG_WASM_CONFIG_DIR})

file(WRITE ${JPEG_WASM_CONFIG_DIR}/jconfig.h
"/* jconfig.h - generated for WASM build */\n\
#define JPEG_LIB_VERSION 62\n\
#define LIBJPEG_TURBO_VERSION 3.0.1\n\
#define LIBJPEG_TURBO_VERSION_NUMBER 3000001\n\
#define HAVE_PROTOTYPES 1\n\
#define HAVE_UNSIGNED_CHAR 1\n\
#define HAVE_UNSIGNED_SHORT 1\n\
#define HAVE_STDDEF_H 1\n\
#define HAVE_STDLIB_H 1\n\
#define HAVE_LOCALE_H 1\n\
#define C_ARITH_CODING_SUPPORTED 1\n\
#define D_ARITH_CODING_SUPPORTED 1\n\
#define MEM_SRCDST_SUPPORTED 1\n\
#ifndef BITS_IN_JSAMPLE\n\
#define BITS_IN_JSAMPLE 8\n\
#endif\n\
#define NEED_SYS_TYPES_H 1\n\
#ifdef JPEG_INTERNALS\n\
#define INLINE inline\n\
#endif\n"
)

file(WRITE ${JPEG_WASM_CONFIG_DIR}/jconfigint.h
"/* jconfigint.h - generated for WASM build */\n\
#define BUILD \"\"\n\
#define INLINE inline\n\
#define THREAD_LOCAL\n\
#define PACKAGE_NAME \"libjpeg-turbo\"\n\
#define VERSION \"3.0.1\"\n\
#define SIZEOF_SIZE_T 4\n\
#define FALLTHROUGH\n\
/* No SIMD / builtin optimizations for WASM */\n\
#undef HAVE_BUILTIN_CTZL\n\
#undef HAVE_INTRIN_H\n\
"
)

file(WRITE ${JPEG_WASM_CONFIG_DIR}/jversion.h
"/* jversion.h - generated for WASM build */\n\
#define JVERSION \"3.0.1\"\n\
#define JCOPYRIGHT \"Copyright (C) the libjpeg-turbo Project\"\n\
#define JCOPYRIGHT_SHORT JCOPYRIGHT\n"
)

# Compile libjpeg-turbo to WASM
set(JPEG_WASM "${CMAKE_BINARY_DIR}/jpeg.wasm")
set(JPEG_H "${CMAKE_BINARY_DIR}/jpeg.wasm.h")
set(JPEG_C "${CMAKE_BINARY_DIR}/jpeg.wasm.c")

set(WASI_CLANG ${wasiclang_SOURCE_DIR}/bin/clang)
set(WASI_SYSROOT ${wasiclang_SOURCE_DIR}/share/wasi-sysroot/)
set(WASI_COMMON_FLAGS --sysroot ${WASI_SYSROOT} -O3 --target=wasm32-wasi
                      -I${jpeg_wasm_SOURCE_DIR} -I${JPEG_WASM_CONFIG_DIR})

# Sources that get recompiled at 12-bit and 16-bit precision
# (matches upstream JPEG12_SOURCES / JPEG16_SOURCES)
set(JPEG16_SOURCES
  ${jpeg_wasm_SOURCE_DIR}/jcapistd.c
  ${jpeg_wasm_SOURCE_DIR}/jccolor.c
  ${jpeg_wasm_SOURCE_DIR}/jcdiffct.c
  ${jpeg_wasm_SOURCE_DIR}/jclossls.c
  ${jpeg_wasm_SOURCE_DIR}/jcmainct.c
  ${jpeg_wasm_SOURCE_DIR}/jcprepct.c
  ${jpeg_wasm_SOURCE_DIR}/jcsample.c
  ${jpeg_wasm_SOURCE_DIR}/jdapistd.c
  ${jpeg_wasm_SOURCE_DIR}/jdcolor.c
  ${jpeg_wasm_SOURCE_DIR}/jddiffct.c
  ${jpeg_wasm_SOURCE_DIR}/jdlossls.c
  ${jpeg_wasm_SOURCE_DIR}/jdmainct.c
  ${jpeg_wasm_SOURCE_DIR}/jdpostct.c
  ${jpeg_wasm_SOURCE_DIR}/jdsample.c
  ${jpeg_wasm_SOURCE_DIR}/jutils.c
)
set(JPEG12_SOURCES ${JPEG16_SOURCES}
  ${jpeg_wasm_SOURCE_DIR}/jccoefct.c
  ${jpeg_wasm_SOURCE_DIR}/jcdctmgr.c
  ${jpeg_wasm_SOURCE_DIR}/jdcoefct.c
  ${jpeg_wasm_SOURCE_DIR}/jddctmgr.c
  ${jpeg_wasm_SOURCE_DIR}/jdmerge.c
  ${jpeg_wasm_SOURCE_DIR}/jfdctfst.c
  ${jpeg_wasm_SOURCE_DIR}/jfdctint.c
  ${jpeg_wasm_SOURCE_DIR}/jidctflt.c
  ${jpeg_wasm_SOURCE_DIR}/jidctfst.c
  ${jpeg_wasm_SOURCE_DIR}/jidctint.c
  ${jpeg_wasm_SOURCE_DIR}/jidctred.c
  ${jpeg_wasm_SOURCE_DIR}/jquant1.c
  ${jpeg_wasm_SOURCE_DIR}/jquant2.c
)

# Core 8-bit sources (superset compiled at default BITS_IN_JSAMPLE=8)
set(JPEG_SOURCES ${JPEG12_SOURCES}
  ${jpeg_wasm_SOURCE_DIR}/jcapimin.c
  ${jpeg_wasm_SOURCE_DIR}/jchuff.c
  ${jpeg_wasm_SOURCE_DIR}/jcicc.c
  ${jpeg_wasm_SOURCE_DIR}/jcinit.c
  ${jpeg_wasm_SOURCE_DIR}/jclhuff.c
  ${jpeg_wasm_SOURCE_DIR}/jcmarker.c
  ${jpeg_wasm_SOURCE_DIR}/jcmaster.c
  ${jpeg_wasm_SOURCE_DIR}/jcomapi.c
  ${jpeg_wasm_SOURCE_DIR}/jcparam.c
  ${jpeg_wasm_SOURCE_DIR}/jcphuff.c
  ${jpeg_wasm_SOURCE_DIR}/jctrans.c
  ${jpeg_wasm_SOURCE_DIR}/jdapimin.c
  ${jpeg_wasm_SOURCE_DIR}/jdatadst.c
  ${jpeg_wasm_SOURCE_DIR}/jdatasrc.c
  ${jpeg_wasm_SOURCE_DIR}/jdhuff.c
  ${jpeg_wasm_SOURCE_DIR}/jdicc.c
  ${jpeg_wasm_SOURCE_DIR}/jdinput.c
  ${jpeg_wasm_SOURCE_DIR}/jdlhuff.c
  ${jpeg_wasm_SOURCE_DIR}/jdmarker.c
  ${jpeg_wasm_SOURCE_DIR}/jdmaster.c
  ${jpeg_wasm_SOURCE_DIR}/jdphuff.c
  ${jpeg_wasm_SOURCE_DIR}/jdtrans.c
  ${jpeg_wasm_SOURCE_DIR}/jerror.c
  ${jpeg_wasm_SOURCE_DIR}/jfdctflt.c
  ${jpeg_wasm_SOURCE_DIR}/jmemmgr.c
  ${jpeg_wasm_SOURCE_DIR}/jmemnobs.c
  ${jpeg_wasm_SOURCE_DIR}/jaricom.c
  ${jpeg_wasm_SOURCE_DIR}/jcarith.c
  ${jpeg_wasm_SOURCE_DIR}/jdarith.c
)

# Intermediate object files for multi-precision builds
set(JPEG12_OBJS "${CMAKE_BINARY_DIR}/jpeg12.o")
set(JPEG16_OBJS "${CMAKE_BINARY_DIR}/jpeg16.o")

# Compile 12-bit precision objects
add_custom_command(OUTPUT "${JPEG12_OBJS}"
                   DEPENDS ${JPEG12_SOURCES}
                   COMMAND ${WASI_CLANG} ${WASI_COMMON_FLAGS}
                           -DBITS_IN_JSAMPLE=12 -DC_LOSSLESS_SUPPORTED -DD_LOSSLESS_SUPPORTED
                           -nostdlib -Wl,-r
                           ${JPEG12_SOURCES}
                           -o ${JPEG12_OBJS}
                   COMMENT "Compiling libjpeg-turbo 12-bit sources to WASM")

# Compile 16-bit precision objects
add_custom_command(OUTPUT "${JPEG16_OBJS}"
                   DEPENDS ${JPEG16_SOURCES}
                   COMMAND ${WASI_CLANG} ${WASI_COMMON_FLAGS}
                           -DBITS_IN_JSAMPLE=16 -DC_LOSSLESS_SUPPORTED -DD_LOSSLESS_SUPPORTED
                           -nostdlib -Wl,-r
                           ${JPEG16_SOURCES}
                           -o ${JPEG16_OBJS}
                   COMMENT "Compiling libjpeg-turbo 16-bit sources to WASM")

# Compile 8-bit sources and link everything into the final .wasm
add_custom_command(OUTPUT "${JPEG_H}" "${JPEG_C}" "${JPEG_WASM}"
                   DEPENDS ${JPEG_SOURCES} ${JPEG12_OBJS} ${JPEG16_OBJS} ${WASM2C_PATH}
                   COMMAND ${WASI_CLANG}
                           --sysroot ${WASI_SYSROOT}
                           -O3
                           -DC_LOSSLESS_SUPPORTED -DD_LOSSLESS_SUPPORTED
                           -Wl,--export-all -Wl,--no-entry -Wl,--growable-table -Wl,--stack-first -Wl,-z,stack-size=1048576 -Wl,--export-table
                           -I${jpeg_wasm_SOURCE_DIR}
                           -I${JPEG_WASM_CONFIG_DIR}
                           -o ${JPEG_WASM}
                           ${JPEG_SOURCES}
                           ${JPEG12_OBJS}
                           ${JPEG16_OBJS}
                   COMMAND ${WASM2C_PATH}
                           -o ${JPEG_C}
                           ${JPEG_WASM}
                   COMMENT "Building libjpeg-turbo as WASM sandboxed library")

################### Build the wasm sandboxed native library ###################

set(WASM2C_RUNTIME_CODE ${wasm2c_compiler_SOURCE_DIR}/wasm2c/wasm-rt-impl.c
                        ${wasm2c_compiler_SOURCE_DIR}/wasm2c/wasm-rt-mem-impl.c
                        ${rlbox_wasm2c_sandbox_SOURCE_DIR}/src/wasm2c_rt_minwasi.c
                        ${rlbox_wasm2c_sandbox_SOURCE_DIR}/src/wasm2c_rt_mem.c)

add_library(jpeg_sandboxed STATIC ${JPEG_C} ${WASM2C_RUNTIME_CODE})
target_include_directories(jpeg_sandboxed PUBLIC ${wasm2c_compiler_SOURCE_DIR}/wasm2c
                                 PUBLIC ${wasm2c_compiler_SOURCE_DIR}/third_party/simde
                                 PUBLIC ${rlbox_wasm2c_sandbox_SOURCE_DIR}/include
                                 PUBLIC ${CMAKE_BINARY_DIR}
                                 PUBLIC ${jpeg_wasm_SOURCE_DIR}
                                 PUBLIC ${JPEG_WASM_CONFIG_DIR})

target_compile_definitions(jpeg_sandboxed PUBLIC WASM_RT_USE_MMAP=1
                                 PUBLIC WASM_RT_SKIP_SIGNAL_RECOVERY=1
                                 PUBLIC WASM_RT_NONCONFORMING_UNCHECKED_STACK_EXHAUSTION=1)


add_executable(main main.cpp)
target_include_directories(main PUBLIC ${rlbox_SOURCE_DIR}/code/include
                                PUBLIC ${rlbox_wasm2c_sandbox_SOURCE_DIR}/include
                                PUBLIC ${jpeg_wasm_SOURCE_DIR}
                                PUBLIC ${JPEG_WASM_CONFIG_DIR}
                                PUBLIC ${CMAKE_BINARY_DIR})
target_link_libraries(main jpeg_sandboxed pthread JPEG::JPEG)

################### Process-sandbox variant ###################
#
# Builds a second binary that runs libjpeg-turbo via rlbox_process_sandbox
# (fork + LD_PRELOAD + RPC) instead of wasm2c.  Pulls shim/build
# infrastructure from the parent rlbox_dynamic_sandbox repo so we don't
# duplicate it.

set(RLBOX_DYNAMIC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(RLBOX_DYNAMIC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${RLBOX_DYNAMIC_ROOT} rlbox_dynamic_parent EXCLUDE_FROM_ALL)

# Child-side wrapper: a long-running process whose dynamic symbol table
# exposes libjpeg-turbo to the shim's dlsym(RTLD_DEFAULT, ...).
# -rdynamic keeps the linker from pruning libjpeg's externally-referenced
# symbols.
add_executable(jpeg_process_wrapper
               ${RLBOX_DYNAMIC_ROOT}/c_src/process_sandbox_wrapper.c)
target_link_libraries(jpeg_process_wrapper PRIVATE JPEG::JPEG)
target_link_options(jpeg_process_wrapper PRIVATE "-rdynamic")

# Host-side main that uses the process sandbox.
add_executable(main_process
               main_process.cpp
               ${RLBOX_DYNAMIC_ROOT}/src/rlbox_process_tls.cpp)
target_include_directories(main_process PRIVATE
                           ${rlbox_SOURCE_DIR}/code/include
                           ${RLBOX_DYNAMIC_ROOT}/include)
target_link_libraries(main_process PRIVATE
                      rlbox_dynamic
                      pthread
                      JPEG::JPEG)
target_compile_definitions(main_process PRIVATE
  JPEG_PROCESS_WRAPPER_PATH="$<TARGET_FILE:jpeg_process_wrapper>")

add_custom_command(TARGET main_process POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E copy_if_different
                           $<TARGET_FILE:sandbox_shim>
                           $<TARGET_FILE_DIR:main_process>/sandbox_shim.so
                   DEPENDS sandbox_shim)
add_dependencies(main_process sandbox_shim jpeg_process_wrapper)

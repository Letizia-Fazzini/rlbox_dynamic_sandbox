cmake_minimum_required(VERSION 3.14)

project(rlbox_dynamic
        VERSION 0.1
        DESCRIPTION "RLBox dynamic sandbox")

# Project Settings ###################

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Whole-project i386 build.  Off by default; turn on to compile every TU
# (host + shim + FetchContent deps) with -m32.  Off-path is the staging
# ground for Config B (host x86_64, shim i386) -- per-target flags will
# eventually replace this when the build is split.
option(RLBOX_BUILD_M32 "Force -m32 on every native target" OFF)
if(RLBOX_BUILD_M32)
  add_compile_options(-m32)
  add_link_options(-m32)
  message(STATUS "rlbox_dynamic: -m32 forced on every target")
endif()

option(RLBOX_DYNAMIC_BUILD_TESTS "Build unit + integration tests" ON)

# Transport selection ###################
#
# The host<->shim wire protocol is pluggable at compile time.  rpclib gives us
# a small, dependency-light baseline (msgpack + asio over TCP loopback).  capnp
# swaps in Cap'n Proto serialization over Unix abstract sockets -- fewer bytes
# on the wire, no event-loop wrapper, and no localhost TCP setup cost.
set(RLBOX_TRANSPORT "capnp" CACHE STRING
    "Wire transport for host<->shim RPC: rpclib | capnp (capnp is the default;
    rpclib remains a supported build but is not included in standard benchmarks)")
set_property(CACHE RLBOX_TRANSPORT PROPERTY STRINGS rpclib capnp)

if(NOT RLBOX_TRANSPORT STREQUAL "rpclib" AND
   NOT RLBOX_TRANSPORT STREQUAL "capnp")
  message(FATAL_ERROR
          "RLBOX_TRANSPORT must be 'rpclib' or 'capnp', got '${RLBOX_TRANSPORT}'")
endif()
message(STATUS "rlbox_dynamic transport: ${RLBOX_TRANSPORT}")

# Seccomp-bpf syscall filter for the worker fork ###################
#
# off     -- no filter installed (default; preserves today's behavior).
# audit   -- SECCOMP_RET_LOG on denied syscalls; library still runs.  Requires
#            'log' in /proc/sys/kernel/seccomp/actions_avail and actions_logged.
# enforce -- SECCOMP_RET_KILL_PROCESS on denied syscalls.  Worker dies; refill
#            loop respawns.
set(RLBOX_SECCOMP "off" CACHE STRING
    "Seccomp filter mode for the worker fork: off | audit | enforce")
set_property(CACHE RLBOX_SECCOMP PROPERTY STRINGS off audit enforce)

if(RLBOX_SECCOMP STREQUAL "off")
  set(_rlbox_seccomp_mode 0)
elseif(RLBOX_SECCOMP STREQUAL "audit")
  set(_rlbox_seccomp_mode 1)
elseif(RLBOX_SECCOMP STREQUAL "enforce")
  set(_rlbox_seccomp_mode 2)
else()
  message(FATAL_ERROR
          "RLBOX_SECCOMP must be 'off', 'audit', or 'enforce', got '${RLBOX_SECCOMP}'")
endif()
message(STATUS "rlbox_dynamic seccomp: ${RLBOX_SECCOMP}")

# Meta-sandbox adaptive policy mode ###################
#
# Default backend-selection strategy returned by make_adaptive_policy().
# global       -- one current_best across all symbols, updated from per-backend
#                 median sums (current behavior).
# per_function -- each symbol decides independently from its own latency rings.
# Embedders that want explicit control can call make_adaptive_global_policy or
# make_adaptive_per_function_policy directly regardless of this setting.
set(RLBOX_META_ADAPTIVE_MODE "global" CACHE STRING
    "Default for make_adaptive_policy: global | per_function")
set_property(CACHE RLBOX_META_ADAPTIVE_MODE PROPERTY STRINGS global per_function)

if(NOT RLBOX_META_ADAPTIVE_MODE STREQUAL "global" AND
   NOT RLBOX_META_ADAPTIVE_MODE STREQUAL "per_function")
  message(FATAL_ERROR
          "RLBOX_META_ADAPTIVE_MODE must be 'global' or 'per_function', got '${RLBOX_META_ADAPTIVE_MODE}'")
endif()
message(STATUS "rlbox_dynamic meta adaptive mode: ${RLBOX_META_ADAPTIVE_MODE}")

# Phase 7c shim arch.  Declared early so rlbox_dynamic INTERFACE defs and the
# downstream sub-build block can both read it.
set(RLBOX_PROCESS_SHIM_ARCH "host" CACHE STRING
    "Arch for sandbox_shim.so + process_sandbox_wrapper: host | i386")
set_property(CACHE RLBOX_PROCESS_SHIM_ARCH PROPERTY STRINGS host i386)

# Dependencies ###################

include(FetchContent)

if(RLBOX_TRANSPORT STREQUAL "rpclib")
  FetchContent_Declare(
    rpclib
    GIT_REPOSITORY https://github.com/rpclib/rpclib.git
    GIT_TAG v2.3.0)
  FetchContent_MakeAvailable(rpclib)
else()
  # Cap'n Proto: prefer system install, fall back to FetchContent.  Building
  # capnp from source is non-trivial (the schema compiler is a real C++ binary)
  # so the system package is much faster when available.
  find_package(CapnProto CONFIG QUIET)
  if(NOT CapnProto_FOUND)
    message(STATUS "system Cap'n Proto not found; fetching v1.0.2")
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(CAPNP_BUILD_TOOLS ON CACHE BOOL "" FORCE)
    # Cap'n Proto's CMake hard-requires gnu extensions for some platform
    # detection; flip this on just for the fetch so our own translation
    # units stay strict.
    set(_rlbox_saved_cxx_ext ${CMAKE_CXX_EXTENSIONS})
    set(CMAKE_CXX_EXTENSIONS ON)
    FetchContent_Declare(
      capnproto
      GIT_REPOSITORY https://github.com/capnproto/capnproto.git
      GIT_TAG v1.0.2
      SOURCE_SUBDIR c++)
    FetchContent_MakeAvailable(capnproto)
    set(CMAKE_CXX_EXTENSIONS ${_rlbox_saved_cxx_ext})
  endif()
endif()

FetchContent_Declare(
  rlbox
  GIT_REPOSITORY https://github.com/PLSysSec/rlbox.git)
FetchContent_MakeAvailable(rlbox)

# Capnp schema codegen ###################

if(RLBOX_TRANSPORT STREQUAL "capnp")
  # Generate <rlbox_process.capnp.h>/<.c++> next to the schema; the header is
  # picked up via the include path on rlbox_dynamic.
  set(RLBOX_CAPNP_SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/include/rlbox_process.capnp)
  set(RLBOX_CAPNP_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/capnp_gen)
  file(MAKE_DIRECTORY ${RLBOX_CAPNP_GEN_DIR})
  set(RLBOX_CAPNP_GEN_HDR ${RLBOX_CAPNP_GEN_DIR}/rlbox_process.capnp.h)
  set(RLBOX_CAPNP_GEN_SRC ${RLBOX_CAPNP_GEN_DIR}/rlbox_process.capnp.c++)
  # Resolve the capnp tool + the C++ plugin.  Both are needed: capnp loads
  # the plugin by name (capnpc-c++) from PATH, but with FetchContent the
  # plugin lives in the build tree, so we hand the absolute path explicitly
  # via the `-o /path/to/plugin` form.
  if(TARGET CapnProto::capnp_tool)
    set(_capnp_tool $<TARGET_FILE:CapnProto::capnp_tool>)
  elseif(TARGET capnp_tool)
    set(_capnp_tool $<TARGET_FILE:capnp_tool>)
  else()
    message(FATAL_ERROR "Cap'n Proto schema compiler target not found")
  endif()
  if(TARGET CapnProto::capnpc_cpp)
    set(_capnp_cpp_plugin $<TARGET_FILE:CapnProto::capnpc_cpp>)
  elseif(TARGET capnpc_cpp)
    set(_capnp_cpp_plugin $<TARGET_FILE:capnpc_cpp>)
  else()
    set(_capnp_cpp_plugin "c++")
  endif()
  # System install ships /capnp/c++.capnp under the include prefix; with
  # FetchContent we have to point the tool at the in-tree copy explicitly.
  if(capnproto_SOURCE_DIR)
    set(_capnp_import_dir ${capnproto_SOURCE_DIR}/c++/src)
  else()
    # System install: rely on the tool's default search path.
    set(_capnp_import_dir "")
  endif()
  set(_capnp_import_arg "")
  if(_capnp_import_dir)
    set(_capnp_import_arg -I${_capnp_import_dir})
  endif()
  add_custom_command(
    OUTPUT ${RLBOX_CAPNP_GEN_HDR} ${RLBOX_CAPNP_GEN_SRC}
    COMMAND ${_capnp_tool} compile -o${_capnp_cpp_plugin}:${RLBOX_CAPNP_GEN_DIR}
            ${_capnp_import_arg}
            --src-prefix=${CMAKE_CURRENT_SOURCE_DIR}/include
            ${RLBOX_CAPNP_SCHEMA}
    DEPENDS ${RLBOX_CAPNP_SCHEMA}
    COMMENT "Generating Cap'n Proto schema for rlbox_process")
  add_library(rlbox_capnp_schema STATIC ${RLBOX_CAPNP_GEN_SRC})
  target_include_directories(rlbox_capnp_schema PUBLIC ${RLBOX_CAPNP_GEN_DIR})
  if(TARGET CapnProto::capnp)
    target_link_libraries(rlbox_capnp_schema PUBLIC CapnProto::capnp)
  else()
    target_link_libraries(rlbox_capnp_schema PUBLIC capnp)
  endif()
endif()

# Core INTERFACE library ###################

add_library(rlbox_dynamic INTERFACE)
target_include_directories(rlbox_dynamic INTERFACE
                           ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${rlbox_SOURCE_DIR}/code/include)
if(RLBOX_TRANSPORT STREQUAL "rpclib")
  target_link_libraries(rlbox_dynamic INTERFACE rpc)
  target_compile_definitions(rlbox_dynamic INTERFACE RLBOX_TRANSPORT_RPCLIB=1)
else()
  target_link_libraries(rlbox_dynamic INTERFACE rlbox_capnp_schema)
  target_compile_definitions(rlbox_dynamic INTERFACE RLBOX_TRANSPORT_CAPNP=1)
endif()
if(RLBOX_META_ADAPTIVE_MODE STREQUAL "per_function")
  target_compile_definitions(rlbox_dynamic INTERFACE RLBOX_META_ADAPTIVE_PER_FUNCTION=1)
endif()

# Contract macro: tells the host-side process backend whether the shim is
# 32-bit or 64-bit, so wire encodings can size pointers/longs accordingly.
if(RLBOX_PROCESS_SHIM_ARCH STREQUAL "i386")
  target_compile_definitions(rlbox_dynamic INTERFACE RLBOX_PROCESS_SHIM_TARGET_BITS=32)
else()
  target_compile_definitions(rlbox_dynamic INTERFACE RLBOX_PROCESS_SHIM_TARGET_BITS=64)
endif()

# --- Sandbox shim (LD_PRELOADed into the child process) ---
#
# The shim overrides malloc/calloc/realloc/free inside the child, running
# allocations through a dlmalloc mspace anchored in the memfd-backed
# shared-memory region.  It also hosts the RPC server the host talks to.
#
# dlmalloc is fetched from Doug Lea's canonical location.  Override with
#   -DDLMALLOC_SOURCE=/path/to/malloc.c
# if you need a vendored copy (e.g. offline builds).

set(DLMALLOC_DEFAULT_URL "https://gee.cs.oswego.edu/pub/misc/malloc.c")
set(DLMALLOC_SOURCE "" CACHE FILEPATH
    "Path to dlmalloc malloc.c.  If empty, downloaded from Doug Lea's site.")

if(NOT DLMALLOC_SOURCE)
  set(DLMALLOC_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/third_party/dlmalloc/malloc.c")
  if(NOT EXISTS "${DLMALLOC_SOURCE}")
    message(STATUS "Downloading dlmalloc from ${DLMALLOC_DEFAULT_URL}")
    file(DOWNLOAD
         "${DLMALLOC_DEFAULT_URL}"
         "${DLMALLOC_SOURCE}"
         TIMEOUT 60
         STATUS DLMALLOC_DL_STATUS
         TLS_VERIFY ON)
    list(GET DLMALLOC_DL_STATUS 0 DLMALLOC_DL_CODE)
    if(NOT DLMALLOC_DL_CODE EQUAL 0)
      message(FATAL_ERROR
              "Failed to download dlmalloc: ${DLMALLOC_DL_STATUS}. "
              "Set -DDLMALLOC_SOURCE=/path/to/malloc.c to bypass.")
    endif()
  endif()
endif()

# Compile dlmalloc as C in mspace-only mode so its global malloc/free do
# not clash with the shim's LD_PRELOAD overrides.
add_library(dlmalloc_mspace OBJECT "${DLMALLOC_SOURCE}")
set_source_files_properties("${DLMALLOC_SOURCE}" PROPERTIES LANGUAGE C)
target_compile_definitions(dlmalloc_mspace PRIVATE
                           ONLY_MSPACES=1
                           USE_LOCKS=1
                           HAVE_MORECORE=0
                           HAVE_MMAP=0)
# dlmalloc triggers a fair number of benign warnings; quiet them so the
# shim build stays readable.
target_compile_options(dlmalloc_mspace PRIVATE
                       -w)

find_library(LIBFFI_LIBRARY ffi REQUIRED)
find_path(LIBFFI_INCLUDE_DIR ffi.h PATH_SUFFIXES ffi)

add_library(sandbox_shim SHARED
            src/rlbox_process_sandbox_shim.cpp
            src/rlbox_process_tls.cpp
            $<TARGET_OBJECTS:dlmalloc_mspace>)
target_include_directories(sandbox_shim PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${LIBFFI_INCLUDE_DIR})
target_link_libraries(sandbox_shim PRIVATE dl pthread ${LIBFFI_LIBRARY})
if(RLBOX_TRANSPORT STREQUAL "rpclib")
  target_link_libraries(sandbox_shim PRIVATE rpc)
  target_compile_definitions(sandbox_shim PRIVATE RLBOX_TRANSPORT_RPCLIB=1)
else()
  target_link_libraries(sandbox_shim PRIVATE rlbox_capnp_schema)
  target_compile_definitions(sandbox_shim PRIVATE RLBOX_TRANSPORT_CAPNP=1)
endif()
target_compile_definitions(sandbox_shim PRIVATE
                           RLBOX_SECCOMP_MODE=${_rlbox_seccomp_mode})
set_target_properties(sandbox_shim PROPERTIES
                      PREFIX ""              # produce "sandbox_shim.so"
                      OUTPUT_NAME sandbox_shim)

# --- Stock process wrapper (no test glue) ---
add_executable(process_sandbox_wrapper c_src/process_sandbox_wrapper.c)

# --- Phase 7c: i386 shim sub-build ---
#
# When RLBOX_PROCESS_SHIM_ARCH=i386, build sandbox_shim.so + the generic
# wrapper as a 32-bit ELF in a separate ExternalProject subtree.  Host
# x86_64 targets continue to be built here unchanged; consumer paths
# (test_process_sandbox, per-library wrappers) flip to consume the i386
# outputs in a follow-up stage.
#
# The sub-build's source tree is shim_i386/CMakeLists.txt.  It re-fetches
# Cap'n Proto (i386 build) but reuses the host's pre-generated capnp
# schema source (the schema is arch-independent C++) and the host's
# already-downloaded dlmalloc malloc.c.

if(RLBOX_PROCESS_SHIM_ARCH STREQUAL "i386")
  include(ExternalProject)
  set(_shim_i386_binary_dir ${CMAKE_BINARY_DIR}/shim_i386_build)

  # The sub-build needs the host's capnp-generated .c++ to already exist on
  # disk before it configures (it skips re-running the schema compiler).
  ExternalProject_Add(shim_i386_subbuild
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shim_i386
    BINARY_DIR ${_shim_i386_binary_dir}
    CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
      -DRLBOX_PARENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -DRLBOX_PARENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -DRLBOX_TRANSPORT=${RLBOX_TRANSPORT}
      -DRLBOX_SECCOMP_MODE=${_rlbox_seccomp_mode}
    BUILD_ALWAYS 1
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS
      ${_shim_i386_binary_dir}/sandbox_shim.so
      ${_shim_i386_binary_dir}/process_sandbox_wrapper
      ${_shim_i386_binary_dir}/zlib_process_wrapper)
  if(RLBOX_TRANSPORT STREQUAL "capnp")
    add_dependencies(shim_i386_subbuild rlbox_capnp_schema)
  endif()

  # Expose the sub-build's outputs as imported targets so the rest of the
  # build can depend on them via $<TARGET_FILE:...> and add_dependencies.
  add_library(sandbox_shim_i386 SHARED IMPORTED GLOBAL)
  set_target_properties(sandbox_shim_i386 PROPERTIES
                        IMPORTED_LOCATION ${_shim_i386_binary_dir}/sandbox_shim.so)
  add_dependencies(sandbox_shim_i386 shim_i386_subbuild)

  add_executable(process_sandbox_wrapper_i386 IMPORTED GLOBAL)
  set_target_properties(process_sandbox_wrapper_i386 PROPERTIES
                        IMPORTED_LOCATION ${_shim_i386_binary_dir}/process_sandbox_wrapper)
  add_dependencies(process_sandbox_wrapper_i386 shim_i386_subbuild)

  add_executable(zlib_process_wrapper_i386 IMPORTED GLOBAL)
  set_target_properties(zlib_process_wrapper_i386 PROPERTIES
                        IMPORTED_LOCATION ${_shim_i386_binary_dir}/zlib_process_wrapper)
  add_dependencies(zlib_process_wrapper_i386 shim_i386_subbuild)
endif()

# --- Tests ###################

if(RLBOX_DYNAMIC_BUILD_TESTS)
  FetchContent_Declare(
    catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v2.13.10)
  FetchContent_MakeAvailable(catch2)

  # Test glue library compiled into a wrapper executable.  -rdynamic makes
  # the glue_* symbols visible to the child's dlsym(RTLD_DEFAULT, ...).
  add_executable(test_sandbox_wrapper
                 c_src/process_sandbox_wrapper.c
                 test/glue_lib.c)
  target_include_directories(test_sandbox_wrapper PRIVATE
                             ${CMAKE_CURRENT_SOURCE_DIR}/test)
  target_link_options(test_sandbox_wrapper PRIVATE "-rdynamic")

  add_executable(test_process_sandbox
                 test/test_main.cpp
                 test/test_mem_alignment.cpp
                 test/test_pointer_ops.cpp
                 test/test_sandbox_integration.cpp
                 src/rlbox_process_tls.cpp)
  target_include_directories(test_process_sandbox PRIVATE
                             ${CMAKE_CURRENT_SOURCE_DIR}/test
                             ${catch2_SOURCE_DIR}/single_include)
  target_link_libraries(test_process_sandbox PRIVATE
                        rlbox_dynamic
                        pthread)
  target_compile_definitions(test_process_sandbox PRIVATE
                             TEST_SANDBOX_WRAPPER_PATH="$<TARGET_FILE:test_sandbox_wrapper>")

  # The sandbox hard-codes LD_PRELOAD="./sandbox_shim.so" (relative), so
  # the test binary must run from the directory that contains the shim.
  # We copy the shim next to the test binary after each build.
  add_custom_command(TARGET test_process_sandbox POST_BUILD
                     COMMAND ${CMAKE_COMMAND} -E copy_if_different
                             $<TARGET_FILE:sandbox_shim>
                             $<TARGET_FILE_DIR:test_process_sandbox>/sandbox_shim.so
                     DEPENDS sandbox_shim)
  # test_process_sandbox depends on the wrapper being built so that
  # TEST_SANDBOX_WRAPPER_PATH is a real file at run-time.
  add_dependencies(test_process_sandbox sandbox_shim test_sandbox_wrapper)

  enable_testing()
  # Register every Catch2 TEST_CASE as its own ctest so success/failure is
  # reported per-test.  catch_discover_tests runs the binary with
  # --list-test-names-only at build time to enumerate cases.
  include(${catch2_SOURCE_DIR}/contrib/Catch.cmake)
  catch_discover_tests(test_process_sandbox
    TEST_PREFIX "process_sandbox: "
    WORKING_DIRECTORY $<TARGET_FILE_DIR:test_process_sandbox>)
endif()

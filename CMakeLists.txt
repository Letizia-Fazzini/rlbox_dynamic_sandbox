cmake_minimum_required(VERSION 3.14)

project(rlbox_dynamic
        VERSION 0.1
        DESCRIPTION "RLBox dynamic sandbox")

# Project Settings ###################

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(RLBOX_DYNAMIC_BUILD_TESTS "Build unit + integration tests" ON)

# Dependencies ###################

include(FetchContent)

FetchContent_Declare(
  rpclib
  GIT_REPOSITORY https://github.com/rpclib/rpclib.git
  GIT_TAG v2.3.0)
FetchContent_MakeAvailable(rpclib)

FetchContent_Declare(
  rlbox
  GIT_REPOSITORY https://github.com/PLSysSec/rlbox.git)
FetchContent_MakeAvailable(rlbox)

# Core INTERFACE library ###################

add_library(rlbox_dynamic INTERFACE)
target_include_directories(rlbox_dynamic INTERFACE
                           ${CMAKE_SOURCE_DIR}/include
                           ${rlbox_SOURCE_DIR}/code/include)
target_link_libraries(rlbox_dynamic INTERFACE rpc)

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
  set(DLMALLOC_SOURCE "${CMAKE_BINARY_DIR}/third_party/dlmalloc/malloc.c")
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
                           HAVE_MMAP=1)
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
                           ${CMAKE_SOURCE_DIR}/include
                           ${LIBFFI_INCLUDE_DIR})
target_link_libraries(sandbox_shim PRIVATE rpc dl pthread ${LIBFFI_LIBRARY})
set_target_properties(sandbox_shim PROPERTIES
                      PREFIX ""              # produce "sandbox_shim.so"
                      OUTPUT_NAME sandbox_shim)

# --- Stock process wrapper (no test glue) ---
add_executable(process_sandbox_wrapper c_src/process_sandbox_wrapper.c)

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
                             ${CMAKE_SOURCE_DIR}/test)
  target_link_options(test_sandbox_wrapper PRIVATE "-rdynamic")

  add_executable(test_process_sandbox
                 test/test_main.cpp
                 test/test_mem_alignment.cpp
                 test/test_pointer_ops.cpp
                 test/test_sandbox_integration.cpp
                 src/rlbox_process_tls.cpp)
  target_include_directories(test_process_sandbox PRIVATE
                             ${CMAKE_SOURCE_DIR}/test
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

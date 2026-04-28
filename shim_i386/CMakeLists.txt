# i386 shim sub-project.  Built via ExternalProject_Add from the top-level
# x86_64 host build.  Self-contained: its own Cap'n Proto FetchContent, its
# own dlmalloc, and its own shim/wrapper executables, all compiled with -m32.
#
# Inputs from the parent build (passed as cache vars on cmake invocation):
#   RLBOX_PARENT_SOURCE_DIR    -- top-level rlbox_dynamic source root
#   RLBOX_PARENT_BINARY_DIR    -- top-level binary dir (where we read the
#                                  pre-generated capnp .h/.c++)
#   RLBOX_TRANSPORT            -- "capnp" or "rpclib" (capnp only is supported
#                                  in i386 path for now; rpclib not wired)
#   RLBOX_SECCOMP_MODE         -- numeric seccomp mode (0/1/2)
#
# Outputs written under ${CMAKE_BINARY_DIR} (the i386 build dir):
#   sandbox_shim.so            -- LD_PRELOADed into the wrapper
#   process_sandbox_wrapper    -- generic wrapper exe (no library link)

cmake_minimum_required(VERSION 3.14)
project(rlbox_dynamic_shim_i386
        VERSION 0.1
        DESCRIPTION "RLBox dynamic sandbox shim (i386)")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Force -m32 on every TU.  Picks up i386 multilib + system i386 libffi.
add_compile_options(-m32)
add_link_options(-m32)

# Required from parent.
if(NOT RLBOX_PARENT_SOURCE_DIR OR NOT RLBOX_PARENT_BINARY_DIR)
  message(FATAL_ERROR
          "shim_i386: must be invoked with RLBOX_PARENT_SOURCE_DIR and "
          "RLBOX_PARENT_BINARY_DIR set (use ExternalProject_Add from the "
          "parent build, do not configure this directory manually)")
endif()

if(NOT RLBOX_TRANSPORT)
  set(RLBOX_TRANSPORT "capnp")
endif()
if(NOT RLBOX_TRANSPORT STREQUAL "capnp")
  message(FATAL_ERROR
          "shim_i386: only capnp transport is supported; got '${RLBOX_TRANSPORT}'")
endif()

if(NOT DEFINED RLBOX_SECCOMP_MODE)
  set(RLBOX_SECCOMP_MODE 0)
endif()

# --- Cap'n Proto (i386, FetchContent) ---
#
# We always FetchContent here -- there is no Ubuntu i386 capnp dev package, and
# the host build's x86_64 capnp can't be linked into an i386 .so.

include(FetchContent)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CAPNP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)  # use parent's host tool
set(WITH_OPENSSL OFF CACHE BOOL "" FORCE)        # avoid x86_64 OpenSSL linkage

# Cap'n Proto requires gnu extensions for some platform detection; flip on
# just for the fetch.
set(_rlbox_saved_cxx_ext ${CMAKE_CXX_EXTENSIONS})
set(CMAKE_CXX_EXTENSIONS ON)
FetchContent_Declare(
  capnproto
  GIT_REPOSITORY https://github.com/capnproto/capnproto.git
  GIT_TAG v1.0.2
  SOURCE_SUBDIR c++)
FetchContent_MakeAvailable(capnproto)
set(CMAKE_CXX_EXTENSIONS ${_rlbox_saved_cxx_ext})

# --- Capnp schema (use parent's pre-generated .h/.c++) ---
#
# Schema generation runs in the parent build (x86_64 capnp tool).  We just
# compile the generated .c++ into the i386 schema lib here.
set(_capnp_gen_dir ${RLBOX_PARENT_BINARY_DIR}/capnp_gen)
set(_capnp_gen_hdr ${_capnp_gen_dir}/rlbox_process.capnp.h)
set(_capnp_gen_src ${_capnp_gen_dir}/rlbox_process.capnp.c++)

add_library(rlbox_capnp_schema STATIC ${_capnp_gen_src})
target_include_directories(rlbox_capnp_schema PUBLIC ${_capnp_gen_dir})
target_link_libraries(rlbox_capnp_schema PUBLIC capnp)

# --- libffi (system i386: /usr/lib/i386-linux-gnu) ---
#
# The host's find_library walks x86_64 paths first, so we explicitly look
# under the i386 multilib dir.
find_library(LIBFFI_I386_LIBRARY ffi
             PATHS /usr/lib/i386-linux-gnu
             NO_DEFAULT_PATH REQUIRED)
find_path(LIBFFI_I386_INCLUDE_DIR ffi.h
          PATHS /usr/include/i386-linux-gnu
          NO_DEFAULT_PATH REQUIRED)

# --- dlmalloc (i386 OBJECT lib) ---
#
# Reuse the parent's already-downloaded malloc.c so we don't re-fetch.
set(_dlmalloc_src ${RLBOX_PARENT_BINARY_DIR}/third_party/dlmalloc/malloc.c)
if(NOT EXISTS "${_dlmalloc_src}")
  message(FATAL_ERROR
          "shim_i386: ${_dlmalloc_src} not found.  The parent x86_64 build "
          "should have downloaded it; rerun the top-level configure first.")
endif()

add_library(dlmalloc_mspace OBJECT "${_dlmalloc_src}")
set_source_files_properties("${_dlmalloc_src}" PROPERTIES LANGUAGE C)
target_compile_definitions(dlmalloc_mspace PRIVATE
                           ONLY_MSPACES=1
                           USE_LOCKS=1
                           HAVE_MORECORE=0
                           HAVE_MMAP=0)
target_compile_options(dlmalloc_mspace PRIVATE -w)

# --- sandbox_shim.so (i386) ---

add_library(sandbox_shim SHARED
            ${RLBOX_PARENT_SOURCE_DIR}/src/rlbox_process_sandbox_shim.cpp
            ${RLBOX_PARENT_SOURCE_DIR}/src/rlbox_process_tls.cpp
            $<TARGET_OBJECTS:dlmalloc_mspace>)
target_include_directories(sandbox_shim PRIVATE
                           ${RLBOX_PARENT_SOURCE_DIR}/include
                           ${LIBFFI_I386_INCLUDE_DIR})
target_link_libraries(sandbox_shim PRIVATE
                      dl pthread
                      ${LIBFFI_I386_LIBRARY}
                      rlbox_capnp_schema)
target_compile_definitions(sandbox_shim PRIVATE
                           RLBOX_TRANSPORT_CAPNP=1
                           RLBOX_SECCOMP_MODE=${RLBOX_SECCOMP_MODE})
set_target_properties(sandbox_shim PROPERTIES
                      PREFIX ""
                      OUTPUT_NAME sandbox_shim)

# --- Generic process wrapper executable (i386) ---

add_executable(process_sandbox_wrapper
               ${RLBOX_PARENT_SOURCE_DIR}/c_src/process_sandbox_wrapper.c)

# --- zlib process wrapper (i386) ---
#
# Per-library wrapper exe whose dynamic symtab exposes zlib to the shim's
# dlsym(RTLD_DEFAULT, ...).  i386 libz lives under multilib; system zlib.h
# is arch-independent.
find_library(LIBZ_I386_LIBRARY z
             PATHS /usr/lib/i386-linux-gnu
             NO_DEFAULT_PATH REQUIRED)

add_executable(zlib_process_wrapper
               ${RLBOX_PARENT_SOURCE_DIR}/c_src/process_sandbox_wrapper.c
               ${RLBOX_PARENT_SOURCE_DIR}/test/zlib-testing/zlib_wrapper.c)
target_link_libraries(zlib_process_wrapper PRIVATE ${LIBZ_I386_LIBRARY})
# -no-pie keeps the i386 wrapper's text segment at the predictable low base
# (~0x08048000) so it can't ASLR-collide with the host-chosen shared region.
target_compile_options(zlib_process_wrapper PRIVATE -fno-pie)
target_link_options(zlib_process_wrapper PRIVATE "-rdynamic" "-no-pie")

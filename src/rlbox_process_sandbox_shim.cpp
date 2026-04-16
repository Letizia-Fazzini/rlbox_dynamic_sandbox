#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <ffi.h>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include "rlbox_process_abi.hpp"
#include "rpc/client.h"
#include "rpc/server.h"

// dlmalloc mspace API declarations
typedef void* mspace;
extern "C"
{
  mspace create_mspace_with_base(void* base, size_t capacity, int locked);
  void* mspace_malloc(mspace msp, size_t bytes);
  void mspace_free(mspace msp, void* mem);
  void* mspace_realloc(mspace msp, void* mem, size_t newsize);
  void* mspace_calloc(mspace msp, size_t n_elements, size_t elem_size);
}

static void* g_shm_base = NULL;
static size_t g_shm_size = 0;
static mspace g_mspace = NULL;
static bool g_in_init = false;
static std::once_flag g_init_once;

// Minimal bootstrap buffer for allocations before SHM is ready
static char g_bootstrap_buf alignas(8)[4096];
static size_t g_bootstrap_offset = 0;

static void init_shm()
{
  g_in_init = true;
  const char* fd_env = getenv("RLBOX_SHM_FD");
  const char* size_env = getenv("RLBOX_SHM_SIZE");
  const char* base_env = getenv("RLBOX_SHM_BASE");

  if (fd_env && size_env && base_env) {
    int fd = atoi(fd_env);
    g_shm_size = atoll(size_env);
    uintptr_t want_base = (uintptr_t)strtoull(base_env, nullptr, 10);
    // Map the memfd at the *same* virtual address the host chose.  This
    // is what keeps pointers consistent: native C code running in the
    // child can write absolute pointers into shared-memory structs and
    // the host reads them back as valid host addresses.  MAP_FIXED_NOREPLACE
    // fails (instead of silently overwriting) if something is already
    // mapped at that range.
    int flags = MAP_SHARED | MAP_FIXED_NOREPLACE;
    void* want_addr = (void*)want_base;
    void* got = mmap(
      want_addr, g_shm_size, PROT_READ | PROT_WRITE, flags, fd, 0);
    if (got == MAP_FAILED || got != want_addr) {
      g_shm_base = NULL;
    } else {
      g_shm_base = got;
      // Initialize dlmalloc mspace. Set locked=1 to ensure thread safety
      // between the RPC server thread and the library main thread.
      g_mspace = create_mspace_with_base(g_shm_base, g_shm_size, 1);
    }
  }
  g_in_init = false;
}

// --- Callback Trampoline Pool ---
// Each trampoline is a real function address that, when called,
// sends an RPC message back to the host.

static std::unique_ptr<rpc::client> g_callback_client;
static constexpr size_t k_callback_slots = 16;
// Plain array rather than std::vector so no allocation-order interaction
// with the malloc override / mspace can leave it in an empty state.
static uintptr_t g_callback_keys[k_callback_slots] = { 0 };

#define TRAMPOLINE_LIST(V)                                                     \
  V(0)                                                                         \
  V(1) V(2) V(3) V(4) V(5) V(6) V(7) V(8) V(9) V(10) V(11) V(12) V(13) V(14)   \
    V(15)

#define DEFINE_TRAMPOLINE(i)                                                   \
  extern "C" int64_t trampoline_##i(                                           \
    int64_t a, int64_t b, int64_t c, int64_t d)                                \
  {                                                                            \
    if (g_callback_client && g_callback_keys[i] != 0) {                        \
      std::vector<int64_t> args = { a, b, c, d };                              \
      auto res =                                                               \
        g_callback_client->call("trigger_callback", g_callback_keys[i], args); \
      return res.as<int64_t>();                                                \
    }                                                                          \
    return 0;                                                                  \
  }

TRAMPOLINE_LIST(DEFINE_TRAMPOLINE)

#define TRAMPOLINE_PTR(i) (void*)trampoline_##i,
static void* g_trampoline_table[] = { TRAMPOLINE_LIST(TRAMPOLINE_PTR) };

extern "C"
{

  void* malloc(size_t size)
  {
    if (size == 0)
      return NULL;

    if (!g_mspace && !g_in_init) {
      std::call_once(g_init_once, init_shm);

      // If not yet initialized (called within init_shm), use bootstrap buffer
      // with a size header for realloc support
      if (!g_mspace) {
        // Ensure total_needed is 8-byte aligned
        size_t total_needed = ((size + 7) & ~7) + sizeof(size_t);
        if (g_bootstrap_offset + total_needed > sizeof(g_bootstrap_buf)) {
          return NULL;
        }
        void* raw = &g_bootstrap_buf[g_bootstrap_offset];
        g_bootstrap_offset += total_needed;
        *(size_t*)raw = size; // Store size for bootstrap realloc
        return (char*)raw + sizeof(size_t);
      }
    }

    return mspace_malloc(g_mspace, size);
  }

  void free(void* ptr)
  {
    if (!ptr)
      return;

    // Check if pointer is in bootstrap buffer
    if ((char*)ptr >= g_bootstrap_buf &&
        (char*)ptr < g_bootstrap_buf + sizeof(g_bootstrap_buf)) {
      return; // Can't free bootstrap memory
    }

    if (g_mspace) {
      mspace_free(g_mspace, ptr);
    }
  }

  void* calloc(size_t nmemb, size_t size)
  {
    // Check for integer overflow before multiplication
    if (nmemb > 0 && size > (size_t)-1 / nmemb) {
      return NULL;
    }

    if (g_mspace) {
      return mspace_calloc(g_mspace, nmemb, size);
    }

    // Falls back to bootstrap + memset
    size_t total = nmemb * size;
    void* ptr = malloc(total);
    if (ptr) {
      memset(ptr, 0, total);
    }
    return ptr;
  }

  void* realloc(void* ptr, size_t size)
  {
    if (!ptr)
      return malloc(size);
    if (size == 0) {
      free(ptr);
      return NULL;
    }

    // Handle realloc from bootstrap buffer
    if ((char*)ptr >= g_bootstrap_buf &&
        (char*)ptr < g_bootstrap_buf + sizeof(g_bootstrap_buf)) {
      size_t old_size = *((size_t*)ptr - 1);
      void* new_ptr = malloc(size);
      if (new_ptr) {
        memcpy(new_ptr, ptr, old_size < size ? old_size : size);
      }
      return new_ptr;
    }

    if (g_mspace) {
      return mspace_realloc(g_mspace, ptr, size);
    }

    return NULL;
  }

} // extern "C"

// --- RPC Server Implementation ---

static void start_rpc_server()
{
  // The host picks both ports by binding to port 0 and passes them here
  // via env vars.  If either is missing we can't communicate with the
  // host — bail out rather than listen on an arbitrary default.
  const char* rpc_port_env = getenv("RLBOX_RPC_PORT");
  const char* cb_port_env = getenv("RLBOX_CALLBACK_PORT");
  if (!rpc_port_env || !cb_port_env) {
    return;
  }
  uint16_t port = static_cast<uint16_t>(atoi(rpc_port_env));
  uint16_t host_callback_port = static_cast<uint16_t>(atoi(cb_port_env));
  if (port == 0 || host_callback_port == 0) {
    return;
  }

  // Client to talk back to the host for callbacks
  g_callback_client =
    std::make_unique<rpc::client>("127.0.0.1", host_callback_port);

  rpc::server srv(port);

  // Handler for symbol lookup.  The returned address is absolute: the
  // host shares the child's VA for shared memory and treats function
  // addresses as raw pointers, which it hands back unchanged in invoke.
  srv.bind("lookup_symbol", [](std::string name) {
    void* ptr = dlsym(RTLD_DEFAULT, name.c_str());
    return (uintptr_t)ptr;
  });

  // Handler for memory allocation.  Reject any pointer outside the shared
  // region — dlmalloc with HAVE_MMAP can satisfy huge requests via its
  // own mmap path, which would land outside the mspace and be unreachable
  // from the host.  Returns the absolute address (same on both sides).
  srv.bind("malloc", [](size_t size) {
    void* ptr = malloc(size);
    if (!ptr || !g_shm_base) {
      return (uintptr_t)0;
    }
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t base = (uintptr_t)g_shm_base;
    if (p < base || p + size > base + g_shm_size) {
      free(ptr);
      return (uintptr_t)0;
    }
    return p;
  });

  // Handler for freeing memory
  srv.bind("free", [](uintptr_t abs_addr) {
    if (abs_addr != 0) {
      free((void*)abs_addr);
    }
  });

  // Map a wire arg_type tag to its libffi type and the storage width the
  // slot occupies on the call frame.  Returns NULL if the tag is unknown.
  auto ffi_type_for_tag = [](int32_t tag) -> ffi_type* {
    switch (tag) {
      case rlbox::ARG_VOID:   return &ffi_type_void;
      case rlbox::ARG_SINT32: return &ffi_type_sint32;
      case rlbox::ARG_UINT32: return &ffi_type_uint32;
      case rlbox::ARG_SINT64: return &ffi_type_sint64;
      case rlbox::ARG_UINT64: return &ffi_type_uint64;
      case rlbox::ARG_POINTER: return &ffi_type_pointer;
      default: return nullptr;
    }
  };

  // Handler for function invocation.
  //
  // Wire schema:
  //   func_addr  : uintptr_t           -- absolute address of the function
  //   ret_tag    : int32_t             -- arg_type for the return slot
  //   arg_tags   : vector<int32_t>     -- one arg_type per argument
  //   arg_values : vector<int64_t>     -- widened int64 slot per argument
  //
  // POINTER-tagged args carry absolute addresses on the wire (host and
  // child share the VA range for shared memory).  The scratch storage for
  // those pointers lives in a parallel vector because ffi_call reads each
  // arg through a void*.
  srv.bind("invoke",
           [ffi_type_for_tag](uintptr_t func_addr,
                              int32_t ret_tag,
                              std::vector<int32_t> arg_tags,
                              std::vector<int64_t> arg_values) -> int64_t {
    if (!g_shm_base || !func_addr) {
      return 0;
    }
    if (arg_tags.size() != arg_values.size()) {
      return 0;
    }

    void* func_ptr = (void*)func_addr;
    size_t nargs = arg_tags.size();

    std::vector<ffi_type*> types(nargs, nullptr);
    std::vector<void*> values(nargs, nullptr);
    // Scratch storage for pointer args.  libffi reads each arg through a
    // void*, so we need stable backing storage per slot.
    std::vector<void*> ptr_storage(nargs, nullptr);

    for (size_t i = 0; i < nargs; ++i) {
      ffi_type* t = ffi_type_for_tag(arg_tags[i]);
      if (!t || arg_tags[i] == rlbox::ARG_VOID) {
        return 0;
      }
      types[i] = t;
      if (arg_tags[i] == rlbox::ARG_POINTER) {
        ptr_storage[i] = (void*)(uintptr_t)arg_values[i];
        values[i] = &ptr_storage[i];
      } else {
        values[i] = &arg_values[i];
      }
    }

    ffi_type* ret_ffi_type = ffi_type_for_tag(ret_tag);
    if (!ret_ffi_type) {
      return 0;
    }

    ffi_cif cif;
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, nargs, ret_ffi_type, types.data()) !=
        FFI_OK) {
      return 0;
    }

    int pipefd[2];
    if (pipe(pipefd) == -1) {
      return 0;
    }

    pid_t pid = fork();
    if (pid == -1) {
      close(pipefd[0]);
      close(pipefd[1]);
      return 0;
    }

    if (pid == 0) {
      // Isolated callee child.
      close(pipefd[0]);
      // libffi writes the return value into a slot of "rtype_sized" bytes,
      // but for our wire representation we always carry an int64 back.
      // A ffi_arg-sized buffer is the portable landing pad for sub-64-bit
      // return widths; zero-initialize so unused bits are defined.
      ffi_arg ret_buf = 0;
      if (ret_tag == rlbox::ARG_VOID) {
        ffi_call(&cif, FFI_FN(func_ptr), nullptr, values.data());
      } else {
        ffi_call(&cif, FFI_FN(func_ptr), &ret_buf, values.data());
      }

      int64_t child_rc = 0;
      switch (ret_tag) {
        case rlbox::ARG_VOID:   child_rc = 0; break;
        case rlbox::ARG_SINT32: child_rc = (int64_t)(int32_t)ret_buf; break;
        case rlbox::ARG_UINT32: child_rc = (int64_t)(uint32_t)ret_buf; break;
        case rlbox::ARG_SINT64: child_rc = (int64_t)ret_buf; break;
        case rlbox::ARG_UINT64: child_rc = (int64_t)(uint64_t)ret_buf; break;
        case rlbox::ARG_POINTER: {
          // Pointer return flows back as an absolute address, which the
          // host can dereference directly thanks to same-base mapping.
          void* p = *(void**)&ret_buf;
          child_rc = (int64_t)(uintptr_t)p;
          break;
        }
      }

      ssize_t written = write(pipefd[1], &child_rc, sizeof(child_rc));
      (void)written;
      close(pipefd[1]);
      _exit(0);
    } else {
      // RPC thread (parent of the callee fork).
      close(pipefd[1]);
      int64_t parent_rc = 0;
      int status;
      if (read(pipefd[0], &parent_rc, sizeof(parent_rc)) <= 0) {
        parent_rc = 0;
      }
      close(pipefd[0]);
      waitpid(pid, &status, 0);
      return parent_rc;
    }
  });

  // Handler for callback registration.  Returns the trampoline's absolute
  // address — the host uses it as a T_PointerType which the sandboxed
  // library then calls directly.
  srv.bind("register_callback", [](uintptr_t host_key) {
    for (size_t i = 0; i < k_callback_slots; ++i) {
      if (g_callback_keys[i] == 0) {
        g_callback_keys[i] = host_key;
        return (uintptr_t)g_trampoline_table[i];
      }
    }
    return (uintptr_t)0;
  });

  // Handler for unregistering callbacks
  srv.bind("unregister_callback", [](uintptr_t host_key) {
    for (size_t i = 0; i < k_callback_slots; ++i) {
      if (g_callback_keys[i] == host_key) {
        g_callback_keys[i] = 0;
        return;
      }
    }
  });

  srv.run();
}

/**
 * This constructor runs automatically when LD_PRELOAD loads this library
 * into the sandboxed process. It spawns the RPC server thread so the
 * process can immediately begin communicating with the RLBox host.
 */
__attribute__((constructor)) static void init_sandbox_shim()
{
  // We start the server in a separate thread so we don't block
  // the main execution of the library/program being sandboxed.
  std::thread rpc_thread(start_rpc_server);
  rpc_thread.detach();
}

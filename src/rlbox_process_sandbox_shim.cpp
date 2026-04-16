#define _GNU_SOURCE
#include <dlfcn.h>
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

  if (fd_env && size_env) {
    int fd = atoi(fd_env);
    g_shm_size = atoll(size_env);
    // Map the shared memory into the child process
    g_shm_base =
      mmap(NULL, g_shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (g_shm_base == MAP_FAILED) {
      g_shm_base = NULL;
    } else {
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

  // Handler for symbol lookup
  srv.bind("lookup_symbol", [](std::string name) {
    void* ptr = dlsym(RTLD_DEFAULT, name.c_str());
    if (!ptr || !g_shm_base)
      return (uintptr_t)0;
    // Return as offset for RLBox pointer translation
    return (uintptr_t)ptr - (uintptr_t)g_shm_base;
  });

  // Handler for memory allocation.  Reject any pointer outside the shared
  // region — dlmalloc with HAVE_MMAP can satisfy huge requests via its
  // own mmap path, which would land outside the mspace and be unreachable
  // from the host.
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
    return p - base;
  });

  // Handler for freeing memory
  srv.bind("free", [](uintptr_t offset) {
    if (g_shm_base && offset != 0) {
      free((char*)g_shm_base + offset);
    }
  });

  // Handler for function invocation
  srv.bind("invoke", [](uintptr_t func_offset, std::vector<int64_t> args) {
    if (!g_shm_base)
      return (int64_t)0;

    void* func_ptr = (char*)g_shm_base + func_offset;
    size_t nargs = args.size();

    // Prepare FFI types and values
    std::vector<ffi_type*> types(nargs, &ffi_type_sint64);
    std::vector<void*> values(nargs);
    for (size_t i = 0; i < nargs; ++i) {
      values[i] = &args[i];
    }

    ffi_cif cif;
    if (ffi_prep_cif(
          &cif, FFI_DEFAULT_ABI, nargs, &ffi_type_sint64, types.data()) ==
        FFI_OK) {

      int pipefd[2];
      if (pipe(pipefd) == -1) {
        return (int64_t)0;
      }

      pid_t pid = fork();
      if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return (int64_t)0;
      }

      if (pid == 0) {
        // Isolated Child Process
        close(pipefd[0]);
        int64_t child_rc = 0;
        ffi_call(&cif, FFI_FN(func_ptr), &child_rc, values.data());
        ssize_t written = write(pipefd[1], &child_rc, sizeof(child_rc));
        (void)written;
        close(pipefd[1]);
        _exit(0);
      } else {
        // RPC Server Thread (Parent)
        close(pipefd[1]);
        int64_t parent_rc = 0;
        int status;
        // Read result from child
        if (read(pipefd[0], &parent_rc, sizeof(parent_rc)) <= 0) {
            parent_rc = 0; // Child likely crashed or failed to write
        }
        close(pipefd[0]);
        waitpid(pid, &status, 0);
        return parent_rc;
      }
    }
    return (int64_t)0;
  });

  // Handler for callback registration
  srv.bind("register_callback", [](uintptr_t host_key) {
    for (size_t i = 0; i < k_callback_slots; ++i) {
      if (g_callback_keys[i] == 0) {
        g_callback_keys[i] = host_key;
        void* t_addr = g_trampoline_table[i];
        return (uintptr_t)t_addr - (uintptr_t)g_shm_base;
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

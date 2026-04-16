#define _GNU_SOURCE
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <dlfcn.h>
#include <errno.h>
#include <ffi.h>
#include <mutex>
#include <new>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "rlbox_process_abi.hpp"

#if !defined(RLBOX_TRANSPORT_RPCLIB) && !defined(RLBOX_TRANSPORT_CAPNP)
#  define RLBOX_TRANSPORT_RPCLIB 1
#endif

#if defined(RLBOX_TRANSPORT_RPCLIB)
#  include "rpc/client.h"
#  include "rpc/server.h"
#elif defined(RLBOX_TRANSPORT_CAPNP)
#  include <capnp/message.h>
#  include <capnp/serialize.h>
#  include <kj/exception.h>
#  include "rlbox_process.capnp.h"
#endif

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

// Forward declaration; defined after the callback array type below.
static void init_shared_callback_keys();

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
      init_shared_callback_keys();
    }
  }
  g_in_init = false;
}

// --- Callback Trampoline Pool ---
// Each trampoline is a real function address that, when called,
// sends an RPC message back to the host.

#if defined(RLBOX_TRANSPORT_RPCLIB)
static std::unique_ptr<rpc::client> g_callback_client;
#elif defined(RLBOX_TRANSPORT_CAPNP)
static int g_callback_fd = -1;
// Workers fire callbacks from independent processes that share g_callback_fd
// via fork inheritance.  A mutex inside one process can't coordinate writes
// across processes, but in practice contention is rare and the pool's worker
// is single-threaded, so the only intra-process collisions come from the
// shim parent's request handler — which we never have firing callbacks.
// We still take this mutex defensively in case a worker becomes
// multi-threaded in the future.
static std::mutex g_callback_fd_mutex;
#endif
static constexpr size_t k_callback_slots = 16;
// The slot array lives in the shared mspace so that pre-forked workers
// (which inherit the *pointer* at fork time) read up-to-date keys via
// shared memory, not their own private snapshot. atomic<uintptr_t> is
// lock-free and standard-layout on x86_64; placement-new initializes
// each slot to 0.
static std::atomic<uintptr_t>* g_callback_keys = nullptr;

static void init_shared_callback_keys()
{
  if (g_callback_keys || !g_mspace) {
    return;
  }
  void* mem = mspace_malloc(
    g_mspace, sizeof(std::atomic<uintptr_t>) * k_callback_slots);
  if (!mem) {
    return;
  }
  auto* slots = static_cast<std::atomic<uintptr_t>*>(mem);
  for (size_t i = 0; i < k_callback_slots; ++i) {
    new (&slots[i]) std::atomic<uintptr_t>(0);
  }
  g_callback_keys = slots;
}

#define TRAMPOLINE_LIST(V)                                                     \
  V(0)                                                                         \
  V(1) V(2) V(3) V(4) V(5) V(6) V(7) V(8) V(9) V(10) V(11) V(12) V(13) V(14)   \
    V(15)

#if defined(RLBOX_TRANSPORT_RPCLIB)
#  define DEFINE_TRAMPOLINE(i)                                                 \
    extern "C" int64_t trampoline_##i(                                         \
      int64_t a, int64_t b, int64_t c, int64_t d)                              \
    {                                                                          \
      if (!g_callback_client || !g_callback_keys) {                            \
        return 0;                                                              \
      }                                                                        \
      uintptr_t key = g_callback_keys[i].load(std::memory_order_acquire);      \
      if (key == 0) {                                                          \
        return 0;                                                              \
      }                                                                        \
      std::vector<int64_t> args = { a, b, c, d };                              \
      auto res = g_callback_client->call("trigger_callback", key, args);       \
      return res.as<int64_t>();                                                \
    }
#elif defined(RLBOX_TRANSPORT_CAPNP)
// Helper: send a CallbackRequest, await CallbackResponse.  Mutex-serialized
// for the same reason capnp_call is on the host side.
static int64_t fire_callback(uintptr_t key,
                             int64_t a,
                             int64_t b,
                             int64_t c,
                             int64_t d)
{
  if (g_callback_fd < 0) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(g_callback_fd_mutex);
  try {
    capnp::MallocMessageBuilder out;
    auto req = out.initRoot<rlbox::wire::CallbackRequest>();
    req.setKey(key);
    auto args = req.initArgs(4);
    args.set(0, a);
    args.set(1, b);
    args.set(2, c);
    args.set(3, d);
    capnp::writeMessageToFd(g_callback_fd, out);
    capnp::StreamFdMessageReader in(g_callback_fd);
    return in.getRoot<rlbox::wire::CallbackResponse>().getResult();
  } catch (const kj::Exception&) {
    return 0;
  }
}

#  define DEFINE_TRAMPOLINE(i)                                                 \
    extern "C" int64_t trampoline_##i(                                         \
      int64_t a, int64_t b, int64_t c, int64_t d)                              \
    {                                                                          \
      if (g_callback_fd < 0 || !g_callback_keys) {                             \
        return 0;                                                              \
      }                                                                        \
      uintptr_t key = g_callback_keys[i].load(std::memory_order_acquire);      \
      if (key == 0) {                                                          \
        return 0;                                                              \
      }                                                                        \
      return fire_callback(key, a, b, c, d);                                   \
    }
#endif

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

// --- libffi dispatch helpers (shared by inline and worker-pool paths) ---

// Map a wire arg_type tag to its libffi type.  Returns NULL if unknown.
static ffi_type* ffi_type_for_tag(int32_t tag)
{
  switch (tag) {
    case rlbox::ARG_VOID:    return &ffi_type_void;
    case rlbox::ARG_SINT32:  return &ffi_type_sint32;
    case rlbox::ARG_UINT32:  return &ffi_type_uint32;
    case rlbox::ARG_SINT64:  return &ffi_type_sint64;
    case rlbox::ARG_UINT64:  return &ffi_type_uint64;
    case rlbox::ARG_POINTER: return &ffi_type_pointer;
    default:                 return nullptr;
  }
}

// Bounded — keeps stack arrays in do_ffi_call sized at compile time
// and gives worker_main something to validate the wire-supplied nargs
// against.  Practical signatures we care about are <10 args.
static constexpr size_t k_max_args = 32;

// Run the actual ffi_call.  Caller is responsible for ensuring the
// surrounding process has the right isolation properties — we either
// invoke this in a freshly-fork()ed child (inline path) or in a
// pre-forked worker that exits immediately afterward (pool path).
//
// All scratch is on the stack: this function is called *after* fork()
// in both paths, and the dlmalloc mspace mutex lives in the shared
// memfd.  If a thread other than the caller held it at fork time, any
// post-fork mspace_malloc in the child would deadlock — so we simply
// don't allocate here.
static int64_t do_ffi_call(uintptr_t func_addr,
                           int32_t ret_tag,
                           const int32_t* arg_tags,
                           const int64_t* arg_values,
                           size_t nargs)
{
  if (!g_shm_base || !func_addr) {
    return 0;
  }
  if (nargs > k_max_args) {
    return 0;
  }

  ffi_type* types[k_max_args];
  void* values[k_max_args];
  void* ptr_storage[k_max_args];
  // Local copy so taking &arg_value_storage[i] yields stable storage
  // independent of the caller's buffer.
  int64_t arg_value_storage[k_max_args];

  for (size_t i = 0; i < nargs; ++i) {
    ffi_type* t = ffi_type_for_tag(arg_tags[i]);
    if (!t || arg_tags[i] == rlbox::ARG_VOID) {
      return 0;
    }
    types[i] = t;
    arg_value_storage[i] = arg_values[i];
    if (arg_tags[i] == rlbox::ARG_POINTER) {
      ptr_storage[i] = (void*)(uintptr_t)arg_values[i];
      values[i] = &ptr_storage[i];
    } else {
      values[i] = &arg_value_storage[i];
    }
  }

  ffi_type* ret_ffi_type = ffi_type_for_tag(ret_tag);
  if (!ret_ffi_type) {
    return 0;
  }

  ffi_cif cif;
  if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, nargs, ret_ffi_type, types) !=
      FFI_OK) {
    return 0;
  }

  // ffi_arg-sized buffer is the portable landing pad for sub-64-bit
  // return widths; zero-initialize so unused bits are defined.
  ffi_arg ret_buf = 0;
  void* func_ptr = (void*)func_addr;
  if (ret_tag == rlbox::ARG_VOID) {
    ffi_call(&cif, FFI_FN(func_ptr), nullptr, values);
  } else {
    ffi_call(&cif, FFI_FN(func_ptr), &ret_buf, values);
  }

  switch (ret_tag) {
    case rlbox::ARG_VOID:    return 0;
    case rlbox::ARG_SINT32:  return (int64_t)(int32_t)ret_buf;
    case rlbox::ARG_UINT32:  return (int64_t)(uint32_t)ret_buf;
    case rlbox::ARG_SINT64:  return (int64_t)ret_buf;
    case rlbox::ARG_UINT64:  return (int64_t)(uint64_t)ret_buf;
    case rlbox::ARG_POINTER: {
      // Pointer return flows back as an absolute address, which the host
      // can dereference directly thanks to same-base mapping.
      void* p = *(void**)&ret_buf;
      return (int64_t)(uintptr_t)p;
    }
  }
  return 0;
}

// --- Pre-forked worker pool ---
//
// The pool hides per-call fork() latency from the critical path while
// preserving the per-call isolation invariant: each invoke still runs
// in a fresh address space that exits immediately after the call (one
// call per child).  Pattern follows SOCK Zygotes (USENIX ATC '18,
// https://www.usenix.org/system/files/conference/atc18/atc18-oakes.pdf)
// and the Android Zygote: fork from a small, already-initialized parent
// off the critical path so only the inherited page-table copy is paid.
//
// Configurable via env var RLBOX_WORKER_POOL_SIZE.  Default 4; setting
// 0 disables the pool and forces every invoke through inline fork (the
// pre-pool behavior, kept as a fallback path).

struct Worker
{
  pid_t pid;
  int req_fd;   // parent writes job here, child reads
  int resp_fd;  // child writes int64 result here, parent reads
};

struct WireJobHeader
{
  uint64_t func_addr;
  int32_t ret_tag;
  uint32_t nargs;
};

static std::mutex g_pool_mutex;
static std::condition_variable g_pool_refill_cv;
static std::deque<Worker> g_pool;
static size_t g_pool_target = 0;
static std::atomic<bool> g_pool_shutdown{ false };

static bool read_full(int fd, void* buf, size_t n)
{
  uint8_t* p = static_cast<uint8_t*>(buf);
  while (n > 0) {
    ssize_t r = read(fd, p, n);
    if (r > 0) {
      p += r;
      n -= (size_t)r;
    } else if (r == 0) {
      return false;
    } else if (errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

static bool write_full(int fd, const void* buf, size_t n)
{
  const uint8_t* p = static_cast<const uint8_t*>(buf);
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w > 0) {
      p += w;
      n -= (size_t)w;
    } else if (errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

// Worker entry point.  Reads exactly one job, runs it, writes the int64
// result, exits.  Anything the call mutates dies with the process.
// Stack arrays only: see do_ffi_call comment about post-fork allocator
// safety.
static void worker_main(int req_fd, int resp_fd)
{
  WireJobHeader hdr;
  if (!read_full(req_fd, &hdr, sizeof(hdr))) {
    _exit(1);
  }
  if (hdr.nargs > k_max_args) {
    _exit(1);
  }
  int32_t arg_tags[k_max_args];
  int64_t arg_values[k_max_args];
  if (hdr.nargs > 0) {
    if (!read_full(req_fd, arg_tags, hdr.nargs * sizeof(int32_t)) ||
        !read_full(req_fd, arg_values, hdr.nargs * sizeof(int64_t))) {
      _exit(1);
    }
  }

  int64_t result = do_ffi_call(
    hdr.func_addr, hdr.ret_tag, arg_tags, arg_values, hdr.nargs);

  (void)write_full(resp_fd, &result, sizeof(result));
  _exit(0);
}

static Worker spawn_worker()
{
  Worker bad{ -1, -1, -1 };
  int req_pipe[2];
  int resp_pipe[2];
  if (pipe(req_pipe) == -1) {
    return bad;
  }
  if (pipe(resp_pipe) == -1) {
    close(req_pipe[0]);
    close(req_pipe[1]);
    return bad;
  }
  pid_t pid = fork();
  if (pid == -1) {
    close(req_pipe[0]);
    close(req_pipe[1]);
    close(resp_pipe[0]);
    close(resp_pipe[1]);
    return bad;
  }
  if (pid == 0) {
    close(req_pipe[1]);
    close(resp_pipe[0]);
    worker_main(req_pipe[0], resp_pipe[1]);
    _exit(0);
  }
  close(req_pipe[0]);
  close(resp_pipe[1]);
  return Worker{ pid, req_pipe[1], resp_pipe[0] };
}

// Pop a ready worker.  Returns Worker with pid==-1 if pool is empty so
// the caller can fall back to inline fork.
static Worker try_acquire_worker()
{
  std::lock_guard<std::mutex> lock(g_pool_mutex);
  if (g_pool.empty()) {
    return Worker{ -1, -1, -1 };
  }
  Worker w = g_pool.front();
  g_pool.pop_front();
  g_pool_refill_cv.notify_one();
  return w;
}

// Send job to worker, read result, reap.  Caller has already removed
// the worker from the pool.
static int64_t dispatch_to_worker(Worker w,
                                  uintptr_t func_addr,
                                  int32_t ret_tag,
                                  const std::vector<int32_t>& arg_tags,
                                  const std::vector<int64_t>& arg_values)
{
  WireJobHeader hdr{ func_addr, ret_tag,
                     static_cast<uint32_t>(arg_tags.size()) };
  bool ok = write_full(w.req_fd, &hdr, sizeof(hdr));
  if (ok && !arg_tags.empty()) {
    ok = write_full(w.req_fd, arg_tags.data(),
                    arg_tags.size() * sizeof(int32_t)) &&
         write_full(w.req_fd, arg_values.data(),
                    arg_values.size() * sizeof(int64_t));
  }
  int64_t result = 0;
  if (ok) {
    if (!read_full(w.resp_fd, &result, sizeof(result))) {
      result = 0;
    }
  }
  close(w.req_fd);
  close(w.resp_fd);
  int status = 0;
  waitpid(w.pid, &status, 0);
  return result;
}

static void refill_loop()
{
  while (!g_pool_shutdown.load()) {
    {
      std::unique_lock<std::mutex> lock(g_pool_mutex);
      g_pool_refill_cv.wait(lock, [] {
        return g_pool.size() < g_pool_target || g_pool_shutdown.load();
      });
      if (g_pool_shutdown.load()) {
        return;
      }
    }
    // fork() outside the lock so concurrent acquires aren't blocked
    // behind the slow page-table copy.
    Worker w = spawn_worker();
    if (w.pid == -1) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(g_pool_mutex);
      g_pool.push_back(w);
    }
  }
}

static void start_worker_pool()
{
  if (!g_mspace) {
    return;  // SHM init failed; pool can't run library calls usefully
  }
  const char* size_env = getenv("RLBOX_WORKER_POOL_SIZE");
  size_t target = 4;
  if (size_env) {
    long parsed = strtol(size_env, nullptr, 10);
    if (parsed < 0) {
      parsed = 0;
    }
    target = (size_t)parsed;
  }
  if (target == 0) {
    return;  // pool disabled; every invoke uses inline fork
  }
  g_pool_target = target;
  std::thread refill(refill_loop);
  refill.detach();
}

// --- Per-operation handlers (transport-agnostic) ---
//
// Both the rpclib and capnp request loops dispatch into these.  Keeping the
// dispatch logic shared means the wire transport is the only thing we're
// actually swapping when comparing.

// dlsym in the child.  The returned address is absolute: the host shares
// the child's VA for shared memory and treats function addresses as raw
// pointers, which it hands back unchanged in invoke.
static uintptr_t handle_lookup_symbol(const char* name)
{
  void* ptr = dlsym(RTLD_DEFAULT, name);
  return (uintptr_t)ptr;
}

// Reject any pointer outside the shared region — dlmalloc with HAVE_MMAP
// can satisfy huge requests via its own mmap path, which would land
// outside the mspace and be unreachable from the host.  Returns the
// absolute address (same on both sides).
static uintptr_t handle_allocate(size_t size)
{
  void* ptr = malloc(size);
  if (!ptr || !g_shm_base) {
    return 0;
  }
  uintptr_t p = (uintptr_t)ptr;
  uintptr_t base = (uintptr_t)g_shm_base;
  if (p < base || p + size > base + g_shm_size) {
    free(ptr);
    return 0;
  }
  return p;
}

static void handle_release(uintptr_t abs_addr)
{
  if (abs_addr != 0) {
    free((void*)abs_addr);
  }
}

// Function invocation.  Dispatch path: try a pre-forked worker; if the
// pool is empty (or disabled via RLBOX_WORKER_POOL_SIZE=0), fall back to
// inline fork so we never block on refill.  Either path preserves the
// one-call-per-child isolation invariant.
//
// POINTER-tagged args carry absolute addresses on the wire (host and child
// share the VA range for shared memory).
static int64_t handle_invoke(uintptr_t func_addr,
                             int32_t ret_tag,
                             const std::vector<int32_t>& arg_tags,
                             const std::vector<int64_t>& arg_values)
{
  if (!g_shm_base || !func_addr) {
    return 0;
  }
  if (arg_tags.size() != arg_values.size()) {
    return 0;
  }
  if (arg_tags.size() > k_max_args) {
    return 0;
  }

  Worker w = try_acquire_worker();
  if (w.pid != -1) {
    return dispatch_to_worker(w, func_addr, ret_tag, arg_tags, arg_values);
  }

  // Fallback: inline fork.  Same isolation as the pool path; just pays
  // the page-table copy on the critical path.  do_ffi_call uses only
  // stack scratch so the post-fork child never touches the mspace
  // mutex (which may have been held by another thread in the parent at
  // fork time).
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
    close(pipefd[0]);
    int64_t child_rc = do_ffi_call(func_addr,
                                   ret_tag,
                                   arg_tags.data(),
                                   arg_values.data(),
                                   arg_tags.size());
    (void)write_full(pipefd[1], &child_rc, sizeof(child_rc));
    close(pipefd[1]);
    _exit(0);
  }
  close(pipefd[1]);
  int64_t parent_rc = 0;
  if (!read_full(pipefd[0], &parent_rc, sizeof(parent_rc))) {
    parent_rc = 0;
  }
  close(pipefd[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  return parent_rc;
}

// Returns the trampoline's absolute address — the host uses it as a
// T_PointerType which the sandboxed library then calls directly.
// compare_exchange races safely against concurrent registrations.
static uintptr_t handle_register_callback(uintptr_t host_key)
{
  if (!g_callback_keys) {
    return 0;
  }
  for (size_t i = 0; i < k_callback_slots; ++i) {
    uintptr_t expected = 0;
    if (g_callback_keys[i].compare_exchange_strong(
          expected, host_key, std::memory_order_release)) {
      return (uintptr_t)g_trampoline_table[i];
    }
  }
  return 0;
}

static void handle_unregister_callback(uintptr_t host_key)
{
  if (!g_callback_keys) {
    return;
  }
  for (size_t i = 0; i < k_callback_slots; ++i) {
    if (g_callback_keys[i].load(std::memory_order_acquire) == host_key) {
      g_callback_keys[i].store(0, std::memory_order_release);
      return;
    }
  }
}

// --- RPC Server Implementation ---

#if defined(RLBOX_TRANSPORT_RPCLIB)
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

  srv.bind("lookup_symbol", [](std::string name) {
    return handle_lookup_symbol(name.c_str());
  });
  srv.bind("malloc", [](size_t size) { return handle_allocate(size); });
  srv.bind("free", [](uintptr_t abs_addr) { handle_release(abs_addr); });
  srv.bind("invoke",
           [](uintptr_t func_addr,
              int32_t ret_tag,
              std::vector<int32_t> arg_tags,
              std::vector<int64_t> arg_values) -> int64_t {
             return handle_invoke(func_addr, ret_tag, arg_tags, arg_values);
           });
  srv.bind("register_callback", [](uintptr_t host_key) {
    return handle_register_callback(host_key);
  });
  srv.bind("unregister_callback", [](uintptr_t host_key) {
    handle_unregister_callback(host_key);
  });

  // Workers can die mid-write if the library aborts; without this an
  // EPIPE on the request pipe would kill the shim parent.
  signal(SIGPIPE, SIG_IGN);

  // Pool start needs g_mspace ready (the shared callback array lives
  // there).  init_shm() is normally lazy via the malloc override but by
  // the time the RPC server is up libc/library constructors have called
  // malloc, so g_mspace is set.  Call it eagerly here as belt-and-
  // suspenders in case a future change defers all allocation.
  std::call_once(g_init_once, init_shm);
  start_worker_pool();

  srv.run();
}
#elif defined(RLBOX_TRANSPORT_CAPNP)
static void start_rpc_server()
{
  // Both fds were created by the host (socketpair) and inherited across
  // exec.  Without them we can't communicate, so bail.
  const char* req_fd_env = getenv("RLBOX_REQ_FD");
  const char* cb_fd_env = getenv("RLBOX_CB_FD");
  if (!req_fd_env || !cb_fd_env) {
    return;
  }
  int req_fd = atoi(req_fd_env);
  g_callback_fd = atoi(cb_fd_env);
  if (req_fd < 0 || g_callback_fd < 0) {
    return;
  }

  signal(SIGPIPE, SIG_IGN);

  std::call_once(g_init_once, init_shm);
  start_worker_pool();

  // Single-threaded request loop.  Each iteration: read one Request,
  // dispatch via the shared handlers, write one Response.  EOF on the
  // request fd (host side closed) ends the loop.
  while (true) {
    capnp::MallocMessageBuilder out;
    auto resp = out.initRoot<rlbox::wire::Response>();
    int64_t result = 0;
    bool reply = true;
    try {
      capnp::StreamFdMessageReader reader(req_fd);
      auto req = reader.getRoot<rlbox::wire::Request>();
      switch (req.which()) {
        case rlbox::wire::Request::LOOKUP_SYMBOL: {
          auto name = req.getLookupSymbol();
          result = (int64_t)handle_lookup_symbol(name.cStr());
          break;
        }
        case rlbox::wire::Request::ALLOCATE: {
          result = (int64_t)handle_allocate(req.getAllocate());
          break;
        }
        case rlbox::wire::Request::RELEASE: {
          handle_release(req.getRelease());
          result = 0;
          break;
        }
        case rlbox::wire::Request::INVOKE: {
          auto inv = req.getInvoke();
          auto tags_in = inv.getArgTags();
          auto vals_in = inv.getArgValues();
          std::vector<int32_t> tags;
          std::vector<int64_t> vals;
          tags.reserve(tags_in.size());
          vals.reserve(vals_in.size());
          for (auto t : tags_in) {
            tags.push_back(t);
          }
          for (auto v : vals_in) {
            vals.push_back(v);
          }
          result = handle_invoke(inv.getFuncAddr(), inv.getRetTag(),
                                 tags, vals);
          break;
        }
        case rlbox::wire::Request::REGISTER_CALLBACK: {
          result = (int64_t)handle_register_callback(req.getRegisterCallback());
          break;
        }
        case rlbox::wire::Request::UNREGISTER_CALLBACK: {
          handle_unregister_callback(req.getUnregisterCallback());
          result = 0;
          break;
        }
        default:
          result = 0;
          break;
      }
    } catch (const kj::Exception&) {
      // EOF or framing error — host has gone away.  Stop the loop.
      reply = false;
      break;
    }
    if (reply) {
      resp.setResult(result);
      try {
        capnp::writeMessageToFd(req_fd, out);
      } catch (const kj::Exception&) {
        break;
      }
    }
  }
}
#endif

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

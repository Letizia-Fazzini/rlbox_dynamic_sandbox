#define _GNU_SOURCE
#include <array>
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
#include <utility>
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
    // Map at the host's chosen VA so absolute pointers stay valid on
    // both sides. NOREPLACE fails fast on collision instead of silently
    // overwriting.
    int flags = MAP_SHARED | MAP_FIXED_NOREPLACE;
    void* want_addr = (void*)want_base;
    void* got = mmap(
      want_addr, g_shm_size, PROT_READ | PROT_WRITE, flags, fd, 0);
    if (got == MAP_FAILED || got != want_addr) {
      g_shm_base = NULL;
    } else {
      g_shm_base = got;
      // locked=1 for thread safety between RPC server thread and library.
      g_mspace = create_mspace_with_base(g_shm_base, g_shm_size, 1);
      init_shared_callback_keys();
    }
  }
  g_in_init = false;
}

// Callback trampoline pool. Each trampoline is a real function address
// that calls back to the host when invoked.

#if defined(RLBOX_TRANSPORT_RPCLIB)
static std::unique_ptr<rpc::client> g_callback_client;
#elif defined(RLBOX_TRANSPORT_CAPNP)
static int g_callback_fd = -1;
// Defensive: worker is single-threaded today, but the mutex guards us if
// a future worker becomes multi-threaded.
static std::mutex g_callback_fd_mutex;
#endif
// Each slot needs a distinct trampoline with a unique address; size fixed
// at build time. Overridable via -DRLBOX_CALLBACK_SLOTS=N.
#ifndef RLBOX_CALLBACK_SLOTS
#  define RLBOX_CALLBACK_SLOTS 64
#endif
static constexpr size_t k_callback_slots = RLBOX_CALLBACK_SLOTS;
// Slot array lives in the shared mspace so pre-forked workers see fresh
// keys through shared memory rather than their private fork snapshot.
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

#if defined(RLBOX_TRANSPORT_CAPNP)
// Send a CallbackRequest and await the response. Mutex-serialized.
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
#endif

// One trampoline per slot index. Each instantiation is a distinct
// function with its own address. SysV x86_64 matches C calling conv, so
// no extern "C" needed; unused trailing args are harmless.
template <size_t I>
static int64_t trampoline_impl(int64_t a, int64_t b, int64_t c, int64_t d)
{
  if (!g_callback_keys) {
    return 0;
  }
#if defined(RLBOX_TRANSPORT_RPCLIB)
  if (!g_callback_client) {
    return 0;
  }
#elif defined(RLBOX_TRANSPORT_CAPNP)
  if (g_callback_fd < 0) {
    return 0;
  }
#endif
  uintptr_t key = g_callback_keys[I].load(std::memory_order_acquire);
  if (key == 0) {
    return 0;
  }
#if defined(RLBOX_TRANSPORT_RPCLIB)
  std::vector<int64_t> args = { a, b, c, d };
  auto res = g_callback_client->call("trigger_callback", key, args);
  return res.as<int64_t>();
#elif defined(RLBOX_TRANSPORT_CAPNP)
  return fire_callback(key, a, b, c, d);
#else
  (void)a; (void)b; (void)c; (void)d;
  return 0;
#endif
}

template <size_t... Is>
static std::array<void*, sizeof...(Is)> make_trampoline_table(
  std::index_sequence<Is...>)
{
  return { reinterpret_cast<void*>(&trampoline_impl<Is>)... };
}

static const std::array<void*, k_callback_slots> g_trampoline_table =
  make_trampoline_table(std::make_index_sequence<k_callback_slots>{});

extern "C"
{

  void* malloc(size_t size)
  {
    if (size == 0)
      return NULL;

    if (!g_mspace && !g_in_init) {
      std::call_once(g_init_once, init_shm);

      // Bootstrap: shm not yet ready (called inside init_shm). Allocate
      // out of g_bootstrap_buf with an inline size header so realloc works.
      if (!g_mspace) {
        size_t total_needed = ((size + 7) & ~7) + sizeof(size_t);
        if (g_bootstrap_offset + total_needed > sizeof(g_bootstrap_buf)) {
          return NULL;
        }
        void* raw = &g_bootstrap_buf[g_bootstrap_offset];
        g_bootstrap_offset += total_needed;
        *(size_t*)raw = size;
        return (char*)raw + sizeof(size_t);
      }
    }

    return mspace_malloc(g_mspace, size);
  }

  void free(void* ptr)
  {
    if (!ptr)
      return;

    // Bootstrap allocations leak; we never reclaim that buffer.
    if ((char*)ptr >= g_bootstrap_buf &&
        (char*)ptr < g_bootstrap_buf + sizeof(g_bootstrap_buf)) {
      return;
    }

    if (g_mspace) {
      mspace_free(g_mspace, ptr);
    }
  }

  void* calloc(size_t nmemb, size_t size)
  {
    if (nmemb > 0 && size > (size_t)-1 / nmemb) {
      return NULL;
    }

    if (g_mspace) {
      return mspace_calloc(g_mspace, nmemb, size);
    }

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

// libffi dispatch helpers shared by inline and worker-pool paths.

// Map a wire arg_type tag to its libffi type. NULL if unknown.
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

// Bounds the stack arrays in do_ffi_call and validates wire nargs.
static constexpr size_t k_max_args = 32;

// Run ffi_call. Caller must ensure isolation -- we run this either in a
// freshly-forked child (inline path) or in a pre-forked worker that
// exits immediately afterward (pool path).
//
// Stack scratch only: the dlmalloc mspace mutex lives in shared memory,
// and post-fork allocation would deadlock if another thread held it at
// fork time. Don't introduce allocations here.
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
  // Local copy so &arg_value_storage[i] is independent of caller's buffer.
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

  // ffi_arg is the portable landing pad for sub-64-bit returns; zero
  // so unused bits are defined.
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
      // Pointer returns flow back as absolute addresses (same-base mapping).
      void* p = *(void**)&ret_buf;
      return (int64_t)(uintptr_t)p;
    }
  }
  return 0;
}

// Pre-forked worker pool. Each worker still runs one call and exits
// (one-call-per-child preserved); the pool just hides fork() latency.
// Configurable via RLBOX_WORKER_POOL_SIZE; 0 disables and falls back
// to inline fork.

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

// Worker entry point: read one job, run it, write the result, exit.
// Stack arrays only -- see do_ffi_call for the post-fork allocator caveat.
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

// Pop a ready worker. Returns pid==-1 on empty so the caller can fall
// back to inline fork.
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

// Send job, read result, reap. Caller has already popped the worker.
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
    // fork() outside the lock so concurrent acquires don't block on
    // the page-table copy.
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
    return;
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
    return;
  }
  g_pool_target = target;
  std::thread refill(refill_loop);
  refill.detach();
}

// Per-operation handlers shared by both transport request loops.

// dlsym in the child. Returns an absolute address; the host hands it back
// unchanged at invoke time thanks to same-base mapping.
static uintptr_t handle_lookup_symbol(const char* name)
{
  void* ptr = dlsym(RTLD_DEFAULT, name);
  return (uintptr_t)ptr;
}

// Reject pointers outside the shared region: dlmalloc with HAVE_MMAP
// can satisfy huge requests outside the mspace, which the host can't
// reach. Returns the absolute address.
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

// Try a pre-forked worker; fall back to inline fork on empty/disabled
// pool. Either path keeps one-call-per-child isolation. POINTER args
// are absolute addresses on the wire (same-base mapping).
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

  // Fallback: inline fork. Same isolation; pays the page-table copy on
  // the critical path.
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

// Returns the trampoline's absolute address; sandboxed library calls
// it directly. compare_exchange races safely against concurrent
// registrations.
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

#if defined(RLBOX_TRANSPORT_RPCLIB)
static void start_rpc_server()
{
  // Host passes both ports via env. Bail if missing.
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

  // Survive worker EPIPE if a library call aborts mid-write.
  signal(SIGPIPE, SIG_IGN);

  // Eager init in case nothing has triggered the malloc override yet.
  std::call_once(g_init_once, init_shm);
  start_worker_pool();

  srv.run();
}
#elif defined(RLBOX_TRANSPORT_CAPNP)
static void start_rpc_server()
{
  // Both fds inherit from the host across exec. Bail if missing.
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

  // Single-threaded request loop. EOF on req_fd ends the loop.
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
      // EOF or framing error: host has gone away.
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

// Runs when LD_PRELOAD loads this library into the sandboxed process.
// Spawns the RPC server on its own thread so it doesn't block the
// library's main execution.
__attribute__((constructor)) static void init_sandbox_shim()
{
  std::thread rpc_thread(start_rpc_server);
  rpc_thread.detach();
}

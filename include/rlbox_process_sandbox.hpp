#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef RLBOX_USE_CUSTOM_SHARED_LOCK
#  include <shared_mutex>
#endif

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

#include "rlbox_helpers.hpp"
#include "rlbox_process_abi.hpp"
#include "rlbox_process_mem.hpp"
#include "rlbox_process_tls.hpp"

namespace rlbox {
class rlbox_process_sandbox
{
public:
  // Child is native Linux x86_64 sharing the host ABI; integer widths
  // mirror the host. T_PointerType is unsigned because rlbox serializes
  // tainted<T*> as a sandbox offset on the wire.
  using T_LongLongType = int64_t;
  using T_LongType = int64_t;
  using T_IntType = int32_t;
  using T_PointerType = uintptr_t;
  using T_ShortType = int16_t;

protected:
  uintptr_t shared_memory_local_base = 0;
  size_t shared_memory_size = 0;
  void* child_process_handle = nullptr;

#if defined(RLBOX_TRANSPORT_RPCLIB)
  uint16_t rpc_port = 0;
  uint16_t callback_port = 0;
  std::unique_ptr<rpc::client> sandbox_client;
  std::unique_ptr<rpc::server> callback_server;
  std::unique_ptr<std::thread> callback_thread;
  std::mutex client_mutex;
#elif defined(RLBOX_TRANSPORT_CAPNP)
  // SOCK_STREAM Unix socket pairs created pre-fork: request (host->shim)
  // and callback (shim->host). Shim picks up the inherited fds via env.
  int request_fd = -1;
  int callback_fd = -1;
  std::mutex request_mutex;
  std::unique_ptr<std::thread> callback_thread;
  std::atomic<bool> callback_thread_stop{ false };
#endif

#if defined(RLBOX_TRANSPORT_RPCLIB)
  // Block until the loopback TCP port accepts connections or the
  // deadline elapses. rpc::client construction is non-blocking, so this
  // gives a crisp "ready" signal before the first real RPC.
  static bool wait_for_tcp_listener(uint16_t port, int timeout_ms)
  {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      int s = ::socket(AF_INET, SOCK_STREAM, 0);
      if (s < 0) {
        return false;
      }
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      addr.sin_port = htons(port);
      int rc =
        ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
      ::close(s);
      if (rc == 0) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
  }

  // Find an unused loopback TCP port by binding to 0 and reading it back.
  // Small TOCTOU between probe and rebind; fine in practice.
  static uint16_t find_free_tcp_port()
  {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
      return 0;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      ::close(sock);
      return 0;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
      ::close(sock);
      return 0;
    }
    ::close(sock);
    return ntohs(addr.sin_port);
  }
#endif

  // Global registry to find sandboxes by pointer
  static inline std::map<uintptr_t, rlbox_process_sandbox*> global_registry;
  static inline std::mutex registry_mutex;

  // Callback dispatchers keyed by host-side unique key. Args are int64
  // wire slots; result is int64 (0 for void).
  std::map<uintptr_t, std::function<int64_t(const std::vector<int64_t>&)>>
    callback_map;
  std::mutex callback_mutex;

#if defined(RLBOX_TRANSPORT_CAPNP)
  // Synchronous request/response over the request socket. Mutex-serialized
  // because SOCK_STREAM doesn't preserve message boundaries.
  int64_t capnp_call(std::function<void(wire::Request::Builder&)> build)
  {
    std::lock_guard<std::mutex> lock(request_mutex);
    if (request_fd < 0) {
      return 0;
    }
    try {
      capnp::MallocMessageBuilder out_msg;
      auto req = out_msg.initRoot<wire::Request>();
      build(req);
      capnp::writeMessageToFd(request_fd, out_msg);
      capnp::StreamFdMessageReader in_msg(request_fd);
      return in_msg.getRoot<wire::Response>().getResult();
    } catch (const kj::Exception&) {
      return 0;
    }
  }

  void start_callback_loop()
  {
    callback_thread = std::make_unique<std::thread>([this]() {
      while (!callback_thread_stop.load()) {
        try {
          capnp::StreamFdMessageReader in_msg(callback_fd);
          auto req = in_msg.getRoot<wire::CallbackRequest>();
          uintptr_t key = req.getKey();
          auto args_list = req.getArgs();
          std::vector<int64_t> args;
          args.reserve(args_list.size());
          for (auto v : args_list) {
            args.push_back(v);
          }
          int64_t result = 0;
          std::function<int64_t(const std::vector<int64_t>&)> dispatcher;
          {
            std::lock_guard<std::mutex> lock(callback_mutex);
            auto it = callback_map.find(key);
            if (it != callback_map.end()) {
              dispatcher = it->second;
            }
          }
          if (dispatcher) {
            // Publish (this, key) so the interceptor inside dispatcher()
            // can recover its context via
            // impl_get_executed_callback_sandbox_and_key.
            detail::thread_local_sandbox = this;
            detail::thread_local_callback_key = reinterpret_cast<void*>(key);
            result = dispatcher(args);
            detail::thread_local_sandbox = nullptr;
            detail::thread_local_callback_key = nullptr;
          }
          capnp::MallocMessageBuilder out_msg;
          out_msg.initRoot<wire::CallbackResponse>().setResult(result);
          capnp::writeMessageToFd(callback_fd, out_msg);
        } catch (const kj::Exception&) {
          return;  // EOF / shim teardown.
        }
      }
    });
  }
#endif

public:
  // Public because rlbox_meta_sandbox forwards through this from a
  // non-friend composition.
  void* impl_lookup_symbol(const char* func_name)
  {
#if defined(RLBOX_TRANSPORT_RPCLIB)
    if (!sandbox_client) {
      return nullptr;
    }
    try {
      auto result =
        sandbox_client->call("lookup_symbol", std::string(func_name));
      return reinterpret_cast<void*>(result.as<T_PointerType>());
    } catch (const std::exception&) {
      return nullptr;
    }
#elif defined(RLBOX_TRANSPORT_CAPNP)
    std::string name(func_name);
    int64_t addr = capnp_call([&](wire::Request::Builder& req) {
      req.setLookupSymbol(name);
    });
    return reinterpret_cast<void*>(static_cast<uintptr_t>(addr));
#endif
  }

protected:
#if defined(RLBOX_TRANSPORT_RPCLIB)
  void start_callback_server(uint16_t port)
  {
    callback_port = port;
    callback_server = std::make_unique<rpc::server>(port);
    callback_server->bind(
      "trigger_callback",
      [this](uintptr_t key, std::vector<int64_t> args) -> int64_t {
        std::function<int64_t(const std::vector<int64_t>&)> dispatcher;
        {
          std::lock_guard<std::mutex> lock(callback_mutex);
          auto it = callback_map.find(key);
          if (it != callback_map.end()) {
            dispatcher = it->second;
          }
        }
        if (dispatcher) {
          // Mirror of the capnp path: publish (this, key) for the
          // interceptor.
          detail::thread_local_sandbox = this;
          detail::thread_local_callback_key = reinterpret_cast<void*>(key);
          int64_t res = dispatcher(args);
          detail::thread_local_sandbox = nullptr;
          detail::thread_local_callback_key = nullptr;
          return res;
        }
        return 0;
      });
    callback_thread =
      std::make_unique<std::thread>([this]() { callback_server->run(); });
  }
#endif

public:
  template<typename T_Char>
  inline bool impl_create_sandbox(const T_Char* library_path)
  {
    // 1. Create shared memory region
    shared_memory_size = 1024 * 1024 * 64; // 64MB example
    int shm_fd = memfd_create("rlbox_shm", 0);
    if (shm_fd == -1)
      return false;
    ftruncate(shm_fd, shared_memory_size);

    // 2. Map it in the host
    // Use aligned mapping to simplify pointer translation and sandbox lookup
    size_t alignment = 0x100000000ull; // 4GB alignment
    void* aligned_addr = os_mmap_aligned(shared_memory_size, alignment);
    if (!aligned_addr) {
      close(shm_fd);
      return false;
    }

    // Now map the memfd into the aligned slot
    void* mmap_res = mmap(aligned_addr,
                          shared_memory_size,
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_FIXED,
                          shm_fd,
                          0);
    if (mmap_res == MAP_FAILED) {
      munmap(aligned_addr, shared_memory_size);
      close(shm_fd);
      return false;
    }

    shared_memory_local_base = reinterpret_cast<uintptr_t>(mmap_res);
    // Set TLS context for this thread
    detail::thread_local_sandbox = this;

    // Register in global registry
    {
      std::lock_guard<std::mutex> lock(registry_mutex);
      global_registry[shared_memory_local_base] = this;
    }

#if defined(RLBOX_TRANSPORT_RPCLIB)
    // Pick free ports for both directions; advertise via env to the shim.
    uint16_t chosen_rpc_port = find_free_tcp_port();
    uint16_t chosen_callback_port = find_free_tcp_port();
    if (chosen_rpc_port == 0 || chosen_callback_port == 0 ||
        chosen_rpc_port == chosen_callback_port) {
      close(shm_fd);
      return false;
    }
    rpc_port = chosen_rpc_port;

    // Start the callback server on a secondary port
    start_callback_server(chosen_callback_port);
#elif defined(RLBOX_TRANSPORT_CAPNP)
    // SOCK_STREAM Unix pairs; Cap'n Proto handles framing. Both ends
    // inherit across fork; the shim reads them from env after exec.
    int req_pair[2];
    int cb_pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, req_pair) != 0) {
      close(shm_fd);
      return false;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, cb_pair) != 0) {
      close(req_pair[0]);
      close(req_pair[1]);
      close(shm_fd);
      return false;
    }
#endif

    pid_t pid = fork();
    if (pid == -1) {
      close(shm_fd);
      return false;
    }

    if (pid == 0) {
      // Child process setup
      setenv("LD_PRELOAD", "./sandbox_shim.so", 1);
      setenv("RLBOX_SHM_FD", std::to_string(shm_fd).c_str(), 1);
      setenv("RLBOX_SHM_SIZE", std::to_string(shared_memory_size).c_str(), 1);
      setenv(
        "RLBOX_SHM_BASE", std::to_string(shared_memory_local_base).c_str(), 1);

#if defined(RLBOX_TRANSPORT_RPCLIB)
      setenv("RLBOX_RPC_PORT", std::to_string(rpc_port).c_str(), 1);
      setenv(
        "RLBOX_CALLBACK_PORT", std::to_string(callback_port).c_str(), 1);
#elif defined(RLBOX_TRANSPORT_CAPNP)
      // Close parent-side ends in the child to keep EOF semantics clean.
      close(req_pair[0]);
      close(cb_pair[0]);
      setenv("RLBOX_REQ_FD", std::to_string(req_pair[1]).c_str(), 1);
      setenv("RLBOX_CB_FD", std::to_string(cb_pair[1]).c_str(), 1);
#endif

      execl(reinterpret_cast<const char*>(library_path),
            reinterpret_cast<const char*>(library_path),
            (char*)nullptr);
      _exit(1);
    }

    // Host no longer needs this copy of the FD
    close(shm_fd);

    // Parent process
    child_process_handle = reinterpret_cast<void*>(static_cast<uintptr_t>(pid));

#if defined(RLBOX_TRANSPORT_RPCLIB)
    // Wait for the child's RPC server before constructing the client;
    // rpc::client connects lazily and won't surface listen failures.
    if (!wait_for_tcp_listener(rpc_port, 5000)) {
      return false;
    }
    try {
      sandbox_client =
        std::make_unique<rpc::client>("127.0.0.1", rpc_port);
    } catch (const std::exception&) {
      return false;
    }
#elif defined(RLBOX_TRANSPORT_CAPNP)
    // Close child-side fds in the parent so EOF-driven shutdown works.
    close(req_pair[1]);
    close(cb_pair[1]);
    request_fd = req_pair[0];
    callback_fd = cb_pair[0];
    start_callback_loop();
#endif
    return true;
  }

  inline void impl_destroy_sandbox()
  {
    if (detail::thread_local_sandbox == this) {
      detail::thread_local_sandbox = nullptr;
    }

    // Remove from global registry
    {
      std::lock_guard<std::mutex> lock(registry_mutex);
      global_registry.erase(shared_memory_local_base);
    }

#if defined(RLBOX_TRANSPORT_RPCLIB)
    sandbox_client.reset();

    if (callback_server) {
      callback_server->stop();
      if (callback_thread && callback_thread->joinable())
        callback_thread->join();
    }
#elif defined(RLBOX_TRANSPORT_CAPNP)
    callback_thread_stop.store(true);
    // Close fds to drive EOF on both ends of both channels.
    if (request_fd >= 0) {
      shutdown(request_fd, SHUT_RDWR);
      close(request_fd);
      request_fd = -1;
    }
    if (callback_fd >= 0) {
      shutdown(callback_fd, SHUT_RDWR);
      close(callback_fd);
      callback_fd = -1;
    }
    if (callback_thread && callback_thread->joinable()) {
      callback_thread->join();
    }
#endif

    if (child_process_handle) {
      pid_t pid =
        static_cast<pid_t>(reinterpret_cast<uintptr_t>(child_process_handle));
      kill(pid, SIGTERM);
      waitpid(pid, nullptr, 0);
      child_process_handle = nullptr;
    }
  }

  // Host and child share memory at the same VA, so a "sandbox-side"
  // pointer is just its absolute address. This is what lets sandbox code
  // write absolute pointers into shared structs and the host read them.
  template<typename T>
  inline void* impl_get_unsandboxed_pointer(T_PointerType p) const
  {
    return reinterpret_cast<void*>(p);
  }

  template<typename T>
  inline T_PointerType impl_get_sandboxed_pointer(const void* p) const
  {
    return reinterpret_cast<T_PointerType>(p);
  }

  template<typename T>
  static inline void* impl_get_unsandboxed_pointer_no_ctx(
    T_PointerType p,
    const void* example_unsandboxed_ptr,
    rlbox_process_sandbox* (*expensive_sandbox_finder)(
      const void* example_unsandboxed_ptr))
  {
    if (p == 0) {
      return nullptr;
    }
    auto sandbox = expensive_sandbox_finder(example_unsandboxed_ptr);
    return sandbox->impl_get_unsandboxed_pointer<T>(p);
  }

  template<typename T>
  static inline T_PointerType impl_get_sandboxed_pointer_no_ctx(
    const void* p,
    const void* example_unsandboxed_ptr,
    rlbox_process_sandbox* (*expensive_sandbox_finder)(
      const void* example_unsandboxed_ptr))
  {
    if (p == 0) {
      return 0;
    }
    auto sandbox = expensive_sandbox_finder(example_unsandboxed_ptr);
    return sandbox->impl_get_sandboxed_pointer<T>(p);
  }

  // Static because rlbox calls this without an instance. Returns true
  // when both pointers are in the same sandbox's shared region, or both
  // are outside every sandbox.
  static inline bool impl_is_in_same_sandbox(const void* p1, const void* p2)
  {
    auto addr1 = reinterpret_cast<uintptr_t>(p1);
    auto addr2 = reinterpret_cast<uintptr_t>(p2);
    std::lock_guard<std::mutex> lock(registry_mutex);
    auto sandbox_of = [&](uintptr_t a) -> rlbox_process_sandbox* {
      for (auto& entry : global_registry) {
        auto base = entry.first;
        auto size = entry.second->shared_memory_size;
        if (a >= base && a < base + size) {
          return entry.second;
        }
      }
      return nullptr;
    };
    return sandbox_of(addr1) == sandbox_of(addr2);
  }
  inline bool impl_is_pointer_in_sandbox_memory(const void* p)
  {
    if (shared_memory_local_base == 0) {
      return false;
    }
    auto ptr = reinterpret_cast<uintptr_t>(p);
    return ptr >= shared_memory_local_base &&
           ptr < shared_memory_local_base + shared_memory_size;
  }
  inline bool impl_is_pointer_in_app_memory(const void* p)
  {
    return !impl_is_pointer_in_sandbox_memory(p);
  }
  inline size_t impl_get_total_memory() { return shared_memory_size; }
  inline void* impl_get_memory_location() const
  {
    return reinterpret_cast<void*>(shared_memory_local_base);
  }

  // Widen an already-converted argument into an int64 wire slot. Pointer
  // args arrive as T_PointerType (sandbox offsets) -- cast directly.
  template<typename A>
  static int64_t pack_slot(A&& v)
  {
    using U = std::remove_cv_t<std::remove_reference_t<A>>;
    if constexpr (std::is_pointer_v<U>) {
      return static_cast<int64_t>(reinterpret_cast<uintptr_t>(v));
    } else {
      return static_cast<int64_t>(v);
    }
  }

  template<typename T, typename T_Converted, typename... T_Args>
  auto impl_invoke_with_func_ptr(T_Converted* func_ptr, T_Args&&... params)
  {
#if defined(RLBOX_TRANSPORT_RPCLIB)
    detail::dynamic_check(sandbox_client != nullptr,
                          "Sandbox not initialized");
#elif defined(RLBOX_TRANSPORT_CAPNP)
    detail::dynamic_check(request_fd >= 0, "Sandbox not initialized");
#endif

    // Recover the original parameter types from T so we can emit
    // ARG_POINTER for real pointer args (rlbox has substituted them
    // away in T_Args / T_Converted).
    using orig_args_tuple = typename abi_detail::function_traits<T>::args_tuple;
    using orig_ret_type = typename abi_detail::function_traits<T>::return_type;

    std::vector<int32_t> arg_tags;
    arg_tags.reserve(sizeof...(T_Args));
    build_tags_from_tuple<orig_args_tuple>(
      arg_tags, std::make_index_sequence<sizeof...(T_Args)>{});

    std::vector<int64_t> arg_values{ pack_slot(std::forward<T_Args>(params))... };

    constexpr int32_t ret_tag = abi_detail::tag_of_v<orig_ret_type>;

#if defined(RLBOX_TRANSPORT_RPCLIB)
    if constexpr (std::is_void_v<orig_ret_type>) {
      sandbox_client->call("invoke",
                           reinterpret_cast<T_PointerType>(func_ptr),
                           ret_tag,
                           arg_tags,
                           arg_values);
    } else {
      auto result =
        sandbox_client->call("invoke",
                             reinterpret_cast<T_PointerType>(func_ptr),
                             ret_tag,
                             arg_tags,
                             arg_values);
      int64_t raw = result.template as<int64_t>();
      // For pointer returns rlbox expects T_PointerType (sandbox offset).
      // For scalars, the wire slot already holds a representable value,
      // so the cast from int64 truncates safely.
      if constexpr (std::is_pointer_v<orig_ret_type>) {
        return static_cast<T_PointerType>(static_cast<uint64_t>(raw));
      } else {
        return static_cast<orig_ret_type>(raw);
      }
    }
#elif defined(RLBOX_TRANSPORT_CAPNP)
    int64_t raw = capnp_call([&](wire::Request::Builder& req) {
      auto inv = req.initInvoke();
      inv.setFuncAddr(reinterpret_cast<T_PointerType>(func_ptr));
      inv.setRetTag(ret_tag);
      auto tags = inv.initArgTags(arg_tags.size());
      for (size_t i = 0; i < arg_tags.size(); ++i) {
        tags.set(i, arg_tags[i]);
      }
      auto values = inv.initArgValues(arg_values.size());
      for (size_t i = 0; i < arg_values.size(); ++i) {
        values.set(i, arg_values[i]);
      }
    });
    if constexpr (std::is_void_v<orig_ret_type>) {
      (void)raw;
      return;
    } else if constexpr (std::is_pointer_v<orig_ret_type>) {
      return static_cast<T_PointerType>(static_cast<uint64_t>(raw));
    } else {
      return static_cast<orig_ret_type>(raw);
    }
#endif
  }

private:
  template<typename Tuple, std::size_t... I>
  static void build_tags_from_tuple(std::vector<int32_t>& out,
                                    std::index_sequence<I...>)
  {
    (out.push_back(
       abi_detail::tag_of_v<std::tuple_element_t<I, Tuple>>),
     ...);
  }

public:

  inline T_PointerType impl_malloc_in_sandbox(size_t size)
  {
#if defined(RLBOX_TRANSPORT_RPCLIB)
    if (!sandbox_client) {
      return 0;
    }
    try {
      // RPC-allocates via dlmalloc in the child; returns an absolute
      // address valid on both sides thanks to same-base mapping.
      auto result = sandbox_client->call("malloc", size);
      return result.as<T_PointerType>();
    } catch (const std::exception&) {
      return 0;
    }
#elif defined(RLBOX_TRANSPORT_CAPNP)
    int64_t raw = capnp_call(
      [&](wire::Request::Builder& req) { req.setAllocate(size); });
    return static_cast<T_PointerType>(static_cast<uint64_t>(raw));
#endif
  }

  inline void impl_free_in_sandbox(T_PointerType p)
  {
#if defined(RLBOX_TRANSPORT_RPCLIB)
    if (!sandbox_client) {
      return;
    }
    sandbox_client->async_call("free", p);
#elif defined(RLBOX_TRANSPORT_CAPNP)
    // Synchronous; keeps framing simple, and the shim's free is cheap.
    (void)capnp_call(
      [&](wire::Request::Builder& req) { req.setRelease(static_cast<uint64_t>(p)); });
#endif
  }

  // Cast an int64 wire slot back into the callback's Nth arg type.
  // Pointer args arrive as sandbox offsets and stay that way.
  template<typename A>
  static A unpack_slot(int64_t raw)
  {
    if constexpr (std::is_pointer_v<A>) {
      return reinterpret_cast<A>(static_cast<uintptr_t>(raw));
    } else {
      return static_cast<A>(raw);
    }
  }

  template<typename T_Ret, typename... T_Args, std::size_t... I>
  static int64_t invoke_callback_unpacked(void* callback,
                                          const std::vector<int64_t>& args,
                                          std::index_sequence<I...>)
  {
    auto fn = reinterpret_cast<T_Ret (*)(T_Args...)>(callback);
    if constexpr (std::is_void_v<T_Ret>) {
      fn(unpack_slot<T_Args>(args[I])...);
      return 0;
    } else if constexpr (std::is_pointer_v<T_Ret>) {
      T_Ret res = fn(unpack_slot<T_Args>(args[I])...);
      return static_cast<int64_t>(reinterpret_cast<uintptr_t>(res));
    } else {
      T_Ret res = fn(unpack_slot<T_Args>(args[I])...);
      return static_cast<int64_t>(res);
    }
  }

  template<typename T_Ret, typename... T_Args>
  inline T_PointerType impl_register_callback(void* key, void* callback)
  {
    {
      std::lock_guard<std::mutex> lock(callback_mutex);
      callback_map[reinterpret_cast<uintptr_t>(key)] =
        [callback](const std::vector<int64_t>& args) -> int64_t {
          return invoke_callback_unpacked<T_Ret, T_Args...>(
            callback, args, std::index_sequence_for<T_Args...>{});
        };
    }

#if defined(RLBOX_TRANSPORT_RPCLIB)
    if (!sandbox_client) {
      return 0;
    }
    try {
      // Sandbox creates a trampoline for the callback key and returns
      // its address (offset) for invocation.
      auto result = sandbox_client->call("register_callback",
                                         reinterpret_cast<uintptr_t>(key));
      return result.template as<T_PointerType>();
    } catch (const std::exception&) {
      return 0;
    }
#elif defined(RLBOX_TRANSPORT_CAPNP)
    int64_t raw = capnp_call([&](wire::Request::Builder& req) {
      req.setRegisterCallback(reinterpret_cast<uintptr_t>(key));
    });
    return static_cast<T_PointerType>(static_cast<uint64_t>(raw));
#endif
  }

  // Recover (sandbox*, key) when a host callback fires. Guarded on the
  // callback key (rather than the sandbox pointer, which is also set by
  // impl_create_sandbox). Returns nulls outside an active callback -- the
  // composing meta uses this as the "process didn't fire" signal.
  static inline std::pair<rlbox_process_sandbox*, void*>
  impl_get_executed_callback_sandbox_and_key()
  {
    if (detail::thread_local_callback_key == nullptr) {
      return { nullptr, nullptr };
    }
    return { detail::thread_local_sandbox,
             detail::thread_local_callback_key };
  }

  template<typename T_Ret, typename... T_Args>
  inline void impl_unregister_callback(void* key)
  {
    {
      std::lock_guard<std::mutex> lock(callback_mutex);
      callback_map.erase(reinterpret_cast<uintptr_t>(key));
    }

#if defined(RLBOX_TRANSPORT_RPCLIB)
    if (sandbox_client) {
      sandbox_client->async_call("unregister_callback",
                                 reinterpret_cast<uintptr_t>(key));
    }
#elif defined(RLBOX_TRANSPORT_CAPNP)
    (void)capnp_call([&](wire::Request::Builder& req) {
      req.setUnregisterCallback(reinterpret_cast<uintptr_t>(key));
    });
#endif
  }
};

} // namespace rlbox

#pragma once

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
#include <sys/wait.h>
#include <unistd.h>

#ifndef RLBOX_USE_CUSTOM_SHARED_LOCK
#  include <shared_mutex>
#endif
#include "rpc/client.h"
#include "rpc/server.h"

#include "rlbox_helpers.hpp"
#include "rlbox_process_abi.hpp"
#include "rlbox_process_mem.hpp"
#include "rlbox_process_tls.hpp"

namespace rlbox {
class rlbox_process_sandbox
{
public:
  // The child is a native Linux x86_64 process sharing the host ABI, so
  // the integer widths mirror the host.  T_PointerType stays as an
  // unsigned integer because rlbox's tainted<T*> is serialized as a
  // sandbox offset on the wire (offset = host_addr - shared_memory_base).
  using T_LongLongType = int64_t;
  using T_LongType = int64_t;
  using T_IntType = int32_t;
  using T_PointerType = uintptr_t;
  using T_ShortType = int16_t;

protected:
  uintptr_t shared_memory_local_base = 0;
  size_t shared_memory_size = 0;
  void* child_process_handle = nullptr;
  uint16_t rpc_port = 0;
  uint16_t callback_port = 0;
  std::unique_ptr<rpc::client> sandbox_client;
  std::unique_ptr<rpc::server> callback_server;
  std::unique_ptr<std::thread> callback_thread;
  std::mutex client_mutex;

  // Block until a loopback TCP port is accepting connections, or the
  // deadline elapses.  Returns true if a connect() succeeded.
  //
  // rpc::client construction is non-blocking — it returns before the
  // server is reachable, so the first real RPC call can fail if the
  // child's RPC thread hasn't yet bound its listen socket.  Probing with a
  // plain connect() here gives us a crisp "ready" signal.
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

  // Ask the kernel for an unused loopback TCP port by binding to port 0 and
  // reading it back via getsockname.  Returns 0 on failure.  A small TOCTOU
  // window exists between this probe and the eventual rebind, but in
  // practice it's plenty for our use and lets multiple sandboxes coexist.
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

  // Global registry to find sandboxes by pointer
  static inline std::map<uintptr_t, rlbox_process_sandbox*> global_registry;
  static inline std::mutex registry_mutex;

  // Callback dispatchers keyed by host-side unique key.  Each dispatcher
  // takes the int64-widened argument slots coming off the wire and returns
  // the callback's result (also widened to int64; 0 for void returns).
  std::map<uintptr_t, std::function<int64_t(const std::vector<int64_t>&)>>
    callback_map;
  std::mutex callback_mutex;

  void* impl_lookup_symbol(const char* func_name)
  {
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
  }

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
          return dispatcher(args);
        }
        return 0;
      });
    callback_thread =
      std::make_unique<std::thread>([this]() { callback_server->run(); });
  }

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

    // Pick free ports for both directions of the RPC connection.  The
    // child-facing RPC port is chosen by the host and handed to the shim
    // via an environment variable; likewise the callback-back-to-host port
    // is bound by us and advertised to the shim.
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
      setenv("RLBOX_RPC_PORT", std::to_string(rpc_port).c_str(), 1);
      setenv(
        "RLBOX_CALLBACK_PORT", std::to_string(callback_port).c_str(), 1);

      execl(reinterpret_cast<const char*>(library_path),
            reinterpret_cast<const char*>(library_path),
            (char*)nullptr);
      _exit(1);
    }

    // Host no longer needs this copy of the FD
    close(shm_fd);

    // Parent process
    child_process_handle = reinterpret_cast<void*>(static_cast<uintptr_t>(pid));

    // Wait for the child's RPC server to actually accept connections
    // before constructing the client; rpc::client does a lazy connect
    // and wouldn't surface "not-yet-listening" as an error.
    if (!wait_for_tcp_listener(rpc_port, 5000)) {
      return false;
    }
    try {
      sandbox_client =
        std::make_unique<rpc::client>("127.0.0.1", rpc_port);
    } catch (const std::exception&) {
      return false;
    }
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

    sandbox_client.reset();

    if (callback_server) {
      callback_server->stop();
      if (callback_thread && callback_thread->joinable())
        callback_thread->join();
    }

    if (child_process_handle) {
      pid_t pid =
        static_cast<pid_t>(reinterpret_cast<uintptr_t>(child_process_handle));
      // Gracefully terminate the child process
      kill(pid, SIGTERM);
      // Wait for the child process to exit to prevent zombies
      waitpid(pid, nullptr, 0);
      child_process_handle = nullptr;
    }
  }

  // Host and child map shared memory at the same virtual address, so the
  // "sandbox-side" representation of a pointer is just its absolute
  // address — no offset math.  This is what lets native C code in the
  // sandbox write absolute pointers into shared structs and have the host
  // read the same bytes back as valid pointers.
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

  // Static because rlbox_sandbox<T_Sbx>::is_in_same_sandbox calls it as
  // T_Sbx::impl_is_in_same_sandbox(...) without an instance.  We resolve
  // each pointer against the global registry and return true only when
  // both land inside the *same* sandbox's shared region.
  // RLBox's memcpy/memset guards use this as "are these two endpoints of
  // a range on the same side of the app/sandbox boundary" — i.e. safe if
  // they're both in the same sandbox OR both outside every sandbox.
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

  // Widen an already-converted argument into an int64 wire slot.  Pointer
  // args arrive as T_PointerType (sandbox offsets) because rlbox converted
  // tainted<T*> before calling us — so we can cast them straight into the
  // slot with no extra translation on the host side.
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
    detail::dynamic_check(sandbox_client != nullptr,
                          "Sandbox not initialized");

    // Recover the original, pre-conversion parameter types from T so we
    // can emit ARG_POINTER for real pointer args (rlbox has already
    // substituted them away in T_Args and T_Converted).
    using orig_args_tuple = typename abi_detail::function_traits<T>::args_tuple;
    using orig_ret_type = typename abi_detail::function_traits<T>::return_type;

    std::vector<int32_t> arg_tags;
    arg_tags.reserve(sizeof...(T_Args));
    build_tags_from_tuple<orig_args_tuple>(
      arg_tags, std::make_index_sequence<sizeof...(T_Args)>{});

    std::vector<int64_t> arg_values{ pack_slot(std::forward<T_Args>(params))... };

    constexpr int32_t ret_tag = abi_detail::tag_of_v<orig_ret_type>;

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
      // rlbox's convert_type expects the sandbox-equivalent representation
      // of the return value.  For pointer returns that's T_PointerType (the
      // sandbox offset).  For scalars the caller's static_cast handles any
      // further narrowing, so returning the original scalar type is enough;
      // the cast from int64 truncates safely because the wire slot already
      // holds a value representable in orig_ret_type.
      if constexpr (std::is_pointer_v<orig_ret_type>) {
        return static_cast<T_PointerType>(static_cast<uint64_t>(raw));
      } else {
        return static_cast<orig_ret_type>(raw);
      }
    }
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
    if (!sandbox_client) {
      return 0;
    }
    try {
      // RPC-allocates via dlmalloc in the child and returns the absolute
      // address, which is valid on both sides thanks to same-base mapping.
      auto result = sandbox_client->call("malloc", size);
      return result.as<T_PointerType>();
    } catch (const std::exception&) {
      return 0;
    }
  }

  inline void impl_free_in_sandbox(T_PointerType p)
  {
    if (!sandbox_client) {
      return;
    }
    sandbox_client->async_call("free", p);
  }

  // Cast an int64 wire slot back into the callback's expected Nth argument
  // type.  Pointer args arrive as sandbox offsets — the sandbox_callback_
  // interceptor on the rlbox side expects T_PointerType for those, so we
  // forward the offset unchanged.
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

    if (!sandbox_client) {
      return 0;
    }

    try {
      // Request the sandbox to create a trampoline for this callback key.
      // The sandbox returns a function pointer (as an offset) that can be
      // called.
      auto result = sandbox_client->call("register_callback",
                                         reinterpret_cast<uintptr_t>(key));
      return result.template as<T_PointerType>();
    } catch (const std::exception&) {
      return 0;
    }
  }

  template<typename T_Ret, typename... T_Args>
  inline void impl_unregister_callback(void* key)
  {
    {
      std::lock_guard<std::mutex> lock(callback_mutex);
      callback_map.erase(reinterpret_cast<uintptr_t>(key));
    }

    if (sandbox_client) {
      sandbox_client->async_call("unregister_callback",
                                 reinterpret_cast<uintptr_t>(key));
    }
  }
};

} // namespace rlbox

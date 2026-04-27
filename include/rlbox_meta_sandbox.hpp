#pragma once

// Meta-sandbox: composes the process and wasm2c backends and dispatches
// each invoke through a policy hook.  Allocations and invokes are routed
// per-call; pointer ownership is encoded in the high bit of T_PointerType
// (bit 31 on i386 hosts, bit 63 on x86_64), so a single tagged value
// identifies which backend owns it.  Per-symbol latency rings let the
// adaptive policy route by observed cost.  Struct-ABI translation runs
// at the invoke boundary when a host-layout allocation crosses into wasm.
//
// Before including, set up the wasm2c preamble exactly as if including
// rlbox_wasm2c_sandbox.hpp directly:
//   #define RLBOX_WASM2C_MODULE_NAME <module>
//   #include "<module>.wasm.h"
// Do NOT define RLBOX_USE_STATIC_CALLS() -- symbols are resolved per
// backend at dispatch time.

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <time.h>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rlbox_process_sandbox.hpp"
#include "rlbox_wasm2c_sandbox.hpp"

#ifndef RLBOX_WASM2C_MODULE_NAME
#  error "rlbox_meta_sandbox.hpp: define RLBOX_WASM2C_MODULE_NAME before include"
#endif

#define RLBOX_META_STRINGIFY_INNER(x) #x
#define RLBOX_META_STRINGIFY(x) RLBOX_META_STRINGIFY_INNER(x)
// Mirrors RLBOX_WASM2C_MODULE_FUNC: $<name>.wasm -> "0x24<name>0x2Ewasm",
// exports as "w2c_<mangled>_<funcname>".  Update if the upstream mangling changes.
#define RLBOX_META_WASM_PREFIX                                                 \
  "w2c_0x24" RLBOX_META_STRINGIFY(RLBOX_WASM2C_MODULE_NAME) "0x2Ewasm_"

namespace rlbox {

// Which underlying backend handles a given invoke.
enum class meta_backend
{
  process,
  wasm,
};

// Per-call context the policy hook sees.  func_name is null on the alloc
// path to mean "deciding where to allocate, not where to dispatch a call";
// latency fields are zero in that case.  Snapshot rather than by-reference
// so the policy never sees stats mutated under it on the hot path.
struct meta_policy_context
{
  const char* func_name;  // resolved symbol name (nullptr for allocation path)
  bool has_wasm = false;  // true iff the symbol resolves on the wasm side
  size_t proc_samples = 0;
  size_t wasm_samples = 0;
  double proc_median_ms = 0.0;
  double wasm_median_ms = 0.0;

  // Aggregate medians across symbols that resolve on both backends and
  // have at least one sample on each.  alloc_size is bytes requested on
  // the alloc path, zero on the invoke path.
  size_t alloc_size = 0;
  double proc_median_sum = 0.0;
  double wasm_median_sum = 0.0;
};

using meta_policy_fn = std::function<meta_backend(const meta_policy_context&)>;

class rlbox_meta_sandbox;  // fwd-decl for the struct translator types below

// Returned by a struct translator on the wasm dispatch path for each
// pointer arg that matches a registered struct allocation.  wasm_scratch
// is the tagged replacement pointer (META_TAG_WASM set); cleanup runs
// post-invoke to sync wasm-layout back to host-layout and free the scratch.
struct meta_struct_translation
{
  uintptr_t wasm_scratch;
  std::function<void()> cleanup;
};

// Translator hook: invoked per-arg when dispatching to wasm, allocates a
// wasm-ABI scratch, copies fields in, and returns the replacement pointer.
using meta_struct_translator_fn = std::function<meta_struct_translation(
  rlbox_meta_sandbox& meta,
  const char* symbol,
  uintptr_t host_ptr)>;

// Adaptive policy with a global `current_best` backend.  Each named
// invoke dispatches to current_best and updates it afterwards: random
// during the first `alloc_warmup` calls, every Nth call after that if
// `reexplore_period` is non-zero, otherwise the backend with the lower
// median latency sum (random on tie).  Alloc invokes follow current_best
// without updating it.
inline meta_policy_fn make_adaptive_policy(size_t alloc_warmup = 1,
                                           size_t reexplore_period = 0)
{
  // shared_ptr so policy copies share counters.
  struct adaptive_state
  {
    meta_backend current_best = meta_backend::process;
    size_t named_invoke_count = 0;
    std::mt19937 rng{ std::random_device{}() };
    std::uniform_int_distribution<int> dist{ 0, 1 };
  };
  auto state = std::make_shared<adaptive_state>();
  return [alloc_warmup, reexplore_period, state](
           const meta_policy_context& ctx) -> meta_backend {
    if (ctx.func_name == nullptr) {
      return state->current_best;
    }

    // Fall back to process for this one call if current_best is wasm but
    // the symbol has no wasm binding; the update still uses normal wins.
    meta_backend result = state->current_best;
    if (result == meta_backend::wasm && !ctx.has_wasm) {
      result = meta_backend::process;
    }

    size_t n = state->named_invoke_count++;
    meta_backend next;
    auto random_backend = [&]() {
      return state->dist(state->rng) == 0 ? meta_backend::process
                                          : meta_backend::wasm;
    };
    if (n < alloc_warmup) {
      next = random_backend();
    } else if (reexplore_period != 0 &&
               ((n - alloc_warmup) % reexplore_period) == 0) {
      next = random_backend();
    } else if (ctx.proc_median_sum == ctx.wasm_median_sum) {
      next = random_backend();
    } else if (ctx.proc_median_sum < ctx.wasm_median_sum) {
      next = meta_backend::process;
    } else {
      next = meta_backend::wasm;
    }
    state->current_best = next;

    return result;
  };
}

// Return-type extractor for a raw C function type.
template<typename F> struct meta_fn_ret;
template<typename R, typename... A> struct meta_fn_ret<R(A...)> { using type = R; };

// Re-derive T_Converted specialised on the wasm backend so the func_ptr
// cast inside wasm2c matches the real wasm ABI; returns are read at
// their true (narrower) width and widened back at the meta boundary.
template<typename F>
using meta_wasm_converted_fn_t = std::remove_pointer_t<decltype(
  ::rlbox::convert_fn_ptr_to_sandbox_equivalent_detail::helper<
    rlbox_wasm2c_sandbox>(std::declval<F*>()))>;

class rlbox_meta_sandbox
{
public:
  // Integer widths follow the process backend (host-native).  Wasm uses
  // narrower widths under its LP32 model, so the wasm-dispatch path
  // re-derives a wasm-specific T_Converted via meta_wasm_converted_fn_t<T>.
  using T_LongLongType = rlbox_process_sandbox::T_LongLongType;
  using T_LongType = rlbox_process_sandbox::T_LongType;
  using T_IntType = rlbox_process_sandbox::T_IntType;
  using T_PointerType = rlbox_process_sandbox::T_PointerType;
  using T_ShortType = rlbox_process_sandbox::T_ShortType;

  // High bit of T_PointerType encodes the owning backend (0 = process,
  // 1 = wasm).  Canonical user-space VAs and wasm offsets both leave it
  // free.  Bit position derived from sizeof so the same source works on
  // 32-bit (bit 31) and 64-bit (bit 63) hosts.
  static constexpr T_PointerType META_TAG_WASM =
    static_cast<T_PointerType>(1)
      << (sizeof(T_PointerType) * CHAR_BIT - 1);
  static constexpr T_PointerType META_TAG_MASK = META_TAG_WASM;

  static constexpr meta_backend tag_owner(T_PointerType p) noexcept
  {
    return (p & META_TAG_MASK) ? meta_backend::wasm : meta_backend::process;
  }
  static constexpr T_PointerType tag_strip(T_PointerType p) noexcept
  {
    return p & ~META_TAG_MASK;
  }
  static constexpr T_PointerType tag_wasm(T_PointerType p) noexcept
  {
    return p | META_TAG_WASM;
  }

protected:
  rlbox_process_sandbox process_sbx;
  rlbox_wasm2c_sandbox wasm_sbx;

public:
  // Exposed for struct-translator hooks: the translator lives in the
  // embedder TU and needs both backends to allocate scratch and resolve VAs.
  rlbox_process_sandbox& get_process_sbx() { return process_sbx; }
  rlbox_wasm2c_sandbox& get_wasm_sbx() { return wasm_sbx; }

protected:

  static double monotonic_ms()
  {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
  }

  // Fixed-size ring of recent latencies (ms).  Small on purpose so the
  // median tracks current steady state, not lifetime averages.
  struct latency_ring
  {
    static constexpr size_t N = 8;
    std::array<double, N> samples{};
    size_t writes = 0;  // total pushes over lifetime of the ring
    void push(double ms)
    {
      samples[writes % N] = ms;
      ++writes;
    }
    size_t size() const { return writes < N ? writes : N; }
    double median() const
    {
      size_t n = size();
      if (n == 0) return 0.0;
      std::array<double, N> copy = samples;
      std::sort(copy.begin(), copy.begin() + n);
      return copy[n / 2 == 0 ? 0 : (n - 1) / 2];
    }
  };

  // One record per looked-up symbol.  Returned as the opaque void* from
  // impl_lookup_symbol; rlbox caches it, so lifetime tracks the sandbox.
  // Holds both backends' resolved pointers and per-backend latency rings.
  struct meta_symbol_record
  {
    std::string name;
    void* process_sym = nullptr;
    void* wasm_sym = nullptr;
    mutable std::mutex stats_mutex;
    latency_ring process_latency;
    latency_ring wasm_latency;

    // Symbol pinning fast-path: after pin_threshold_ consecutive slow-path
    // dispatches to the same backend, pinned flips to 1=process / 2=wasm
    // and subsequent invokes skip policy + ownership + sample push.
    // consecutive_same_picks/last_pick are guarded by stats_mutex.
    std::atomic<uint8_t> pinned{0};
    uint32_t consecutive_same_picks = 0;
    meta_backend last_pick = meta_backend::process;
  };

  std::mutex symbol_cache_mutex;
  std::unordered_map<std::string, std::unique_ptr<meta_symbol_record>>
    symbol_cache;

  // Per-allocation ownership registry: keyed by tagged T_PointerType so
  // process VAs and wasm offsets can't collide.  Used by the invoke path
  // to override the policy when a pointer arg is owned by a different backend.
  std::mutex alloc_mutex;
  std::unordered_map<T_PointerType, meta_backend> alloc_owner;

  // Struct-ABI translation registry.  struct_types holds one entry per
  // registered struct type; alloc-time disambiguation is by host_size, so
  // colliding sizes abort with a diagnostic.  struct_allocs maps each live
  // tagged host allocation to its stable co-allocated wasm scratch -- stable
  // because zlib's deflate_state caches strm and checks it on every call.
  struct meta_struct_type_info
  {
    std::string symbol;
    size_t host_size;
    size_t wasm_size;
    meta_struct_translator_fn translator;
  };
  struct meta_struct_alloc_info
  {
    uint32_t wasm_scratch;
    meta_struct_translator_fn translator;
  };
  std::vector<meta_struct_type_info> struct_types;
  std::unordered_map<T_PointerType, meta_struct_alloc_info> struct_allocs;

  // Dispatch counters for tests/benchmarks; reflect the post-override
  // choice, not the raw policy pick.
  size_t mallocs_on_process_ = 0;
  size_t mallocs_on_wasm_ = 0;
  size_t invokes_on_process_ = 0;
  size_t invokes_on_wasm_ = 0;

  // Eager dual callback registration: each register_callback installs on
  // both backends.  Keyed by the process trampoline address (also the
  // meta-level handle returned to the caller); used to rewrite handle
  // args into wasm slot indices when dispatching to wasm.
  struct meta_callback_record
  {
    rlbox_wasm2c_sandbox::T_PointerType wasm_slot;
    void* key;
  };
  std::mutex callback_mutex;
  std::unordered_map<T_PointerType, meta_callback_record> callback_registry;

  meta_policy_fn policy;

  // Slow-path dispatches in a row to the same backend before pinning kicks
  // in.  0 disables pinning entirely.  Once pinned, the policy isn't
  // consulted, so re-exploration won't fire for that symbol.
  uint32_t pin_threshold_ = 0;

public:
  rlbox_meta_sandbox()
    : policy([](const meta_policy_context&) { return meta_backend::process; })
  {}

  // Override the per-call dispatch policy.  Called on every invoke.
  void set_policy(meta_policy_fn new_policy)
  {
    policy = std::move(new_policy);
  }

  // Maps the process backend's TLS-published rlbox_process_sandbox* back
  // to the owning meta sandbox.  Process callbacks dispatch on a dedicated
  // host thread, so the invoker's TLS doesn't reach them; wasm callbacks
  // are synchronous on the invoker thread and don't need this.
  static std::mutex& meta_instance_mutex()
  {
    static std::mutex m;
    return m;
  }
  static std::unordered_map<rlbox_process_sandbox*, rlbox_meta_sandbox*>&
    process_to_meta_map()
  {
    static std::unordered_map<rlbox_process_sandbox*, rlbox_meta_sandbox*> m;
    return m;
  }

  // TLS so a wasm-side callback firing mid-invoke can identify the meta.
  // Set on entry to the wasm branch of impl_invoke_with_func_ptr (RAII).
  static inline thread_local rlbox_meta_sandbox* tl_invoking_meta = nullptr;

  // Bring up both backends; if either fails, tear the other down so the
  // meta is "both up or neither up".
  template<typename T_Char>
  inline bool impl_create_sandbox(const T_Char* library_path)
  {
    if (!wasm_sbx.impl_create_sandbox()) {
      return false;
    }
    if (!process_sbx.impl_create_sandbox(library_path)) {
      wasm_sbx.impl_destroy_sandbox();
      return false;
    }
    {
      std::lock_guard<std::mutex> g(meta_instance_mutex());
      process_to_meta_map()[&process_sbx] = this;
    }
    return true;
  }

  inline void impl_destroy_sandbox()
  {
    {
      std::lock_guard<std::mutex> g(meta_instance_mutex());
      process_to_meta_map().erase(&process_sbx);
    }
    wasm_sbx.impl_destroy_sandbox();
    process_sbx.impl_destroy_sandbox();
  }

  template<typename T>
  inline void* impl_get_unsandboxed_pointer(T_PointerType p) const
  {
    if (p == 0) {
      return nullptr;
    }
    if (p & META_TAG_MASK) {
      // Wasm-owned: strip and narrow before the wasm resolver.
      return wasm_sbx.impl_get_unsandboxed_pointer<T>(
        static_cast<rlbox_wasm2c_sandbox::T_PointerType>(tag_strip(p)));
    }
    // Untagged: may still be a wasm offset that lost its tag on the way
    // through a sandbox memory slot.  Probe wasm's linear memory range
    // first; process VAs are always above it in practice.
    auto& ws = const_cast<rlbox_wasm2c_sandbox&>(wasm_sbx);
    if (static_cast<size_t>(p) < ws.impl_get_total_memory()) {
      return wasm_sbx.impl_get_unsandboxed_pointer<T>(
        static_cast<rlbox_wasm2c_sandbox::T_PointerType>(p));
    }
    return process_sbx.impl_get_unsandboxed_pointer<T>(p);
  }

  template<typename T>
  inline T_PointerType impl_get_sandboxed_pointer(const void* p) const
  {
    // Returns the backend-native T_PointerType (no tag).  Tagged values
    // narrowed into a wasm slot would trap OOB; ownership tracking still
    // works because alloc_owner accepts tagged or untagged keys.
    // const_cast because wasm_sbx's membership check is non-const upstream.
    auto& ws = const_cast<rlbox_wasm2c_sandbox&>(wasm_sbx);
    if (ws.impl_is_pointer_in_sandbox_memory(p)) {
      auto raw = wasm_sbx.impl_get_sandboxed_pointer<T>(p);
      return static_cast<T_PointerType>(raw);
    }
    return process_sbx.impl_get_sandboxed_pointer<T>(p);
  }

  template<typename T>
  static inline void* impl_get_unsandboxed_pointer_no_ctx(
    T_PointerType p,
    const void* example_unsandboxed_ptr,
    rlbox_meta_sandbox* (*expensive_sandbox_finder)(
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
    rlbox_meta_sandbox* (*expensive_sandbox_finder)(
      const void* example_unsandboxed_ptr))
  {
    if (p == 0) {
      return 0;
    }
    auto sandbox = expensive_sandbox_finder(example_unsandboxed_ptr);
    return sandbox->impl_get_sandboxed_pointer<T>(p);
  }

  // 3-arg form: locates the owning meta via finder, then disambiguates
  // process-side vs wasm-side within the same meta.  Can't forward to
  // process's 2-arg version because that would call (host, wasm) "same".
  static inline bool impl_is_in_same_sandbox(
    const void* p1,
    const void* p2,
    rlbox_meta_sandbox* (*expensive_sandbox_finder)(
      const void* example_sandbox_ptr))
  {
    // find_sandbox_from_example dynamic_checks non-null; guard for the
    // null-tolerant contract.
    if (p1 == nullptr && p2 == nullptr) {
      return true;
    }
    auto* s1 = p1 ? expensive_sandbox_finder(p1) : nullptr;
    auto* s2 = p2 ? expensive_sandbox_finder(p2) : nullptr;
    if (s1 != s2) {
      return false;
    }
    if (s1 == nullptr) {
      return true;  // both app memory
    }
    bool p1_in_wasm = s1->get_wasm_sbx().impl_is_pointer_in_sandbox_memory(p1);
    bool p2_in_wasm = s2->get_wasm_sbx().impl_is_pointer_in_sandbox_memory(p2);
    return p1_in_wasm == p2_in_wasm;
  }

  inline bool impl_is_pointer_in_sandbox_memory(const void* p)
  {
    if (process_sbx.impl_is_pointer_in_sandbox_memory(p)) {
      return true;
    }
    return wasm_sbx.impl_is_pointer_in_sandbox_memory(p);
  }

  inline bool impl_is_pointer_in_app_memory(const void* p)
  {
    return !impl_is_pointer_in_sandbox_memory(p);
  }

  inline size_t impl_get_total_memory()
  {
    return process_sbx.impl_get_total_memory();
  }

  inline void* impl_get_memory_location() const
  {
    return process_sbx.impl_get_memory_location();
  }

  // Resolve the symbol in *both* backends and return a stable handle.
  // RLBox caches this handle per call site, so the two lookups happen
  // once per symbol for the life of the sandbox.  Wasm lookup is allowed
  // to return nullptr (wrapper-only symbols, or symbols not present in
  // the wasm module); a policy that routes to wasm for such a name is
  // a configuration bug and will abort in impl_invoke_with_func_ptr.
  void* impl_lookup_symbol(const char* func_name)
  {
    std::lock_guard<std::mutex> g(symbol_cache_mutex);
    auto it = symbol_cache.find(func_name);
    if (it != symbol_cache.end()) {
      return it->second.get();
    }
    auto rec = std::make_unique<meta_symbol_record>();
    rec->name = func_name;
    rec->process_sym = process_sbx.impl_lookup_symbol(func_name);
    // wasm2c's impl_lookup_symbol is a static_assert trap (it forces
    // RLBOX_USE_STATIC_CALLS on, which we can't use here -- see CLAUDE.md).
    // Instead resolve the generated thunk directly: wasm2c emits each
    // export as a symbol named "w2c_<module>_<name>" in the host binary,
    // and zlib_sandboxed is linked with -rdynamic equivalents so dlsym
    // finds them.  Returns nullptr for symbols not exported by the wasm
    // module (e.g. host-only wrappers like deflateInitWrapper).
    std::string wasm_name = RLBOX_META_WASM_PREFIX;
    wasm_name += func_name;
    rec->wasm_sym = dlsym(RTLD_DEFAULT, wasm_name.c_str());
    auto* raw = rec.get();
    symbol_cache.emplace(rec->name, std::move(rec));
    return raw;
  }

  // Wasm-path arg rewrite.  If `a` is a T_PointerType that matches a
  // registered callback handle (the process trampoline address returned
  // from impl_register_callback), swap in the paired wasm slot index so
  // wasm's call_indirect hits the right interceptor.  Non-pointer args
  // and non-callback pointer values (including tagged wasm offsets and
  // ordinary process VAs) pass through unchanged.  Only invoked on the
  // wasm dispatch path -- the process path passes handles through
  // verbatim because that's what process_sbx returned.
  template<typename A>
  A rewrite_for_wasm(A a)
  {
    if constexpr (std::is_same_v<std::remove_cv_t<A>, T_PointerType>) {
      if (a != 0) {
        std::lock_guard<std::mutex> g(callback_mutex);
        auto it = callback_registry.find(static_cast<T_PointerType>(a));
        if (it != callback_registry.end()) {
          return static_cast<A>(it->second.wasm_slot);
        }
      }
    }
    return a;
  }

  template<typename T, typename T_Converted, typename... T_Args>
  auto impl_invoke_with_func_ptr(T_Converted* func_ptr, T_Args&&... params)
  {
    auto* rec = reinterpret_cast<meta_symbol_record*>(func_ptr);

    // Snapshot under stats_mutex so the policy sees a coherent
    // (count, median) pair per backend.
    meta_policy_context ctx{};
    ctx.func_name = rec->name.c_str();
    ctx.has_wasm = (rec->wasm_sym != nullptr);
    {
      std::lock_guard<std::mutex> g(rec->stats_mutex);
      ctx.proc_samples = rec->process_latency.size();
      ctx.wasm_samples = rec->wasm_latency.size();
      ctx.proc_median_ms = rec->process_latency.median();
      ctx.wasm_median_ms = rec->wasm_latency.median();
    }
    snapshot_median_sums(ctx.proc_median_sum, ctx.wasm_median_sum);
    meta_backend choice = policy(ctx);

    // Pointer-ownership override: a pointer arg's owning backend wins
    // over the policy pick.  Mixed ownership has no valid route -> abort.
    bool saw_process_ptr = false;
    bool saw_wasm_ptr = false;
    auto inspect = [&](auto&& a) {
      using A = std::remove_cv_t<std::remove_reference_t<decltype(a)>>;
      if constexpr (std::is_same_v<A, T_PointerType>) {
        T_PointerType p = static_cast<T_PointerType>(a);
        if (p != 0) {
          auto [found, owner] = owner_of(p);
          if (found) {
            if (owner == meta_backend::process) saw_process_ptr = true;
            else saw_wasm_ptr = true;
          }
        }
      }
    };
    (inspect(std::forward<T_Args>(params)), ...);

    if (saw_process_ptr && saw_wasm_ptr) {
      fputs("rlbox_meta_sandbox: invoke args span both backends' allocations\n",
            stderr);
      std::abort();
    }
    if (saw_process_ptr) choice = meta_backend::process;
    else if (saw_wasm_ptr) choice = meta_backend::wasm;

    // RAII sample pusher: times the forwarded call and updates pin state
    // under the same lock hold.  Works for void and non-void uniformly.
    struct sample_pusher
    {
      meta_symbol_record* rec;
      meta_backend which;
      double t0;
      uint32_t pin_threshold;
      ~sample_pusher()
      {
        double t1 = monotonic_ms();
        std::lock_guard<std::mutex> g(rec->stats_mutex);
        if (which == meta_backend::process) {
          rec->process_latency.push(t1 - t0);
        } else {
          rec->wasm_latency.push(t1 - t0);
        }
        if (pin_threshold > 0) {
          if (rec->last_pick == which) {
            ++rec->consecutive_same_picks;
            if (rec->consecutive_same_picks >= pin_threshold) {
              rec->pinned.store(which == meta_backend::process ? 1 : 2,
                                std::memory_order_release);
            }
          } else {
            rec->consecutive_same_picks = 1;
            rec->last_pick = which;
          }
        }
      }
    };

    if (choice == meta_backend::wasm) {
      if (rec->wasm_sym == nullptr) {
        fputs("rlbox_meta_sandbox: policy chose wasm for symbol with no wasm "
              "binding\n",
              stderr);
        std::abort();
      }
      invokes_on_wasm_++;
      // Per-arg translate (pointer args only): registered struct alloc
      // -> run translator and push cleanup; registered callback -> swap
      // for wasm slot index; otherwise strip the tag and pass through.
      std::vector<std::function<void()>> struct_cleanups;
      const char* sym_name = rec->name.c_str();
      auto translate = [&](auto&& a) {
        using A = std::remove_cv_t<std::remove_reference_t<decltype(a)>>;
        if constexpr (std::is_same_v<A, T_PointerType>) {
          A val = static_cast<A>(a);
          if (val != 0) {
            meta_struct_translator_fn alloc_translator;
            {
              std::lock_guard<std::mutex> g(alloc_mutex);
              auto sit = struct_allocs.find(val);
              if (sit != struct_allocs.end() && sit->second.translator) {
                alloc_translator = sit->second.translator;
              }
            }
            if (alloc_translator) {
              auto t = alloc_translator(*this, sym_name,
                                        static_cast<uintptr_t>(val));
              if (t.cleanup) {
                struct_cleanups.push_back(std::move(t.cleanup));
              }
              return static_cast<A>(t.wasm_scratch);
            }

            std::lock_guard<std::mutex> g(callback_mutex);
            auto it = callback_registry.find(val);
            if (it != callback_registry.end()) {
              return static_cast<A>(it->second.wasm_slot);
            }
          }
          // Explicit strip: on x86_64 the uintptr_t->uint32_t narrow
          // clears bit 63 anyway, but on i386 the narrow is the identity
          // and bit 31 would land in wasm as a >2GiB OOB offset.
          return static_cast<A>(tag_strip(val));
        } else {
          return static_cast<A>(std::forward<decltype(a)>(a));
        }
      };

      // sp destroyed *after* cleanup_runner (reverse decl order), so the
      // latency sample covers the full host->wasm->host round-trip.
      struct cleanup_runner
      {
        std::vector<std::function<void()>>* cleanups;
        ~cleanup_runner()
        {
          if (cleanups) {
            for (auto& c : *cleanups) c();
          }
        }
      };
      // RAII-stash the invoking meta so a wasm callback firing mid-invoke
      // can recover it via tl_invoking_meta.
      struct meta_scope
      {
        rlbox_meta_sandbox* prev;
        ~meta_scope() { tl_invoking_meta = prev; }
      };
      meta_scope ms{ tl_invoking_meta };
      tl_invoking_meta = this;
      sample_pusher sp{ rec, meta_backend::wasm, monotonic_ms(),
                        pin_threshold_ };
      cleanup_runner cr{ &struct_cleanups };
      // Wasm-ABI re-typing of the function pointer.  See
      // meta_wasm_converted_fn_t comment for the why; in short, the
      // T_Converted rlbox handed us is computed against the meta's
      // (=process's) T_*Type widths and would cause wasm2c to read
      // returns / pass args at the wrong width.  T_Converted_Wasm is
      // the same function type but with each T_*Type taken from
      // rlbox_wasm2c_sandbox, so the func_ptr cast inside wasm2c
      // matches the actual wasm thunk's signature.
      using T_Converted_Wasm = meta_wasm_converted_fn_t<T>;
      using T_Ret_wasm = typename meta_fn_ret<T>::type;
      if constexpr (std::is_pointer_v<T_Ret_wasm>) {
        auto raw = wasm_sbx.template impl_invoke_with_func_ptr<T, T_Converted_Wasm>(
          reinterpret_cast<T_Converted_Wasm*>(rec->wasm_sym),
          translate(std::forward<T_Args>(params))...);
        if (raw != 0) {
          T_PointerType tagged = tag_wasm(static_cast<T_PointerType>(raw));
          {
            std::lock_guard<std::mutex> g(alloc_mutex);
            alloc_owner.emplace(tagged, meta_backend::wasm);
          }
          return tagged;
        }
        return static_cast<T_PointerType>(0);
      } else {
        // Widen wasm's narrower return to T_Converted's so this branch
        // and the process branch deduce a consistent `auto` return type,
        // and rlbox's return-conversion machinery sees the bit-width it
        // expects.
        using T_Ret_conv = typename meta_fn_ret<T_Converted>::type;
        return static_cast<T_Ret_conv>(
          wasm_sbx.template impl_invoke_with_func_ptr<T, T_Converted_Wasm>(
            reinterpret_cast<T_Converted_Wasm*>(rec->wasm_sym),
            translate(std::forward<T_Args>(params))...));
      }
    }

    invokes_on_process_++;
    sample_pusher sp{ rec, meta_backend::process, monotonic_ms(),
                      pin_threshold_ };
    using T_Ret_proc = typename meta_fn_ret<T>::type;
    if constexpr (std::is_pointer_v<T_Ret_proc>) {
      auto result = process_sbx.impl_invoke_with_func_ptr<T, T_Converted>(
        reinterpret_cast<T_Converted*>(rec->process_sym),
        std::forward<T_Args>(params)...);
      if (result != 0) {
        std::lock_guard<std::mutex> g(alloc_mutex);
        alloc_owner.emplace(result, meta_backend::process);
      }
      return result;
    } else {
      // See the matching cast on the wasm branch above.
      using T_Ret_conv = typename meta_fn_ret<T_Converted>::type;
      return static_cast<T_Ret_conv>(
        process_sbx.impl_invoke_with_func_ptr<T, T_Converted>(
          reinterpret_cast<T_Converted*>(rec->process_sym),
          std::forward<T_Args>(params)...));
    }
  }

  // Sum per-backend medians across symbols that resolve on both sides
  // and have a sample on each.  Off the invoke hot path; never nests
  // symbol_cache_mutex inside any record's stats_mutex.
  void snapshot_median_sums(double& proc_median_sum, double& wasm_median_sum)
  {
    proc_median_sum = 0.0;
    wasm_median_sum = 0.0;
    std::vector<meta_symbol_record*> records;
    {
      std::lock_guard<std::mutex> g(symbol_cache_mutex);
      records.reserve(symbol_cache.size());
      for (auto& kv : symbol_cache) {
        if (kv.second->wasm_sym != nullptr) {
          records.push_back(kv.second.get());
        }
      }
    }
    for (auto* rec : records) {
      std::lock_guard<std::mutex> g(rec->stats_mutex);
      if (rec->process_latency.size() == 0) continue;
      if (rec->wasm_latency.size() == 0) continue;
      double pm = rec->process_latency.median();
      double wm = rec->wasm_latency.median();
      proc_median_sum += pm;
      wasm_median_sum += wm;
    }
  }

  inline T_PointerType impl_malloc_in_sandbox(size_t size)
  {
    // Policy is invoked with func_name == nullptr to mean "alloc, not
    // invoke", with aggregate medians across the symbol cache so an
    // adaptive policy can bias toward the faster backend overall.
    meta_policy_context ctx{};
    ctx.func_name = nullptr;
    ctx.alloc_size = size;
    snapshot_median_sums(ctx.proc_median_sum, ctx.wasm_median_sum);
    meta_backend choice = policy(ctx);

    T_PointerType p;
    if (choice == meta_backend::wasm) {
      auto raw = wasm_sbx.impl_malloc_in_sandbox(size);  // uint32_t offset
      if (raw == 0) {
        return 0;
      }
      p = tag_wasm(static_cast<T_PointerType>(raw));
      mallocs_on_wasm_++;
    } else {
      p = process_sbx.impl_malloc_in_sandbox(size);
      if (p == 0) {
        return 0;
      }
      // Process pointers must never set the tag bit; only fires if the
      // shared region passes 2 GiB on 32-bit / 8 EiB on 64-bit hosts.
      if (p & META_TAG_MASK) {
        fputs("rlbox_meta_sandbox: process allocation collided with tag bit\n",
              stderr);
        std::abort();
      }
      mallocs_on_process_++;
    }

    {
      std::lock_guard<std::mutex> g(alloc_mutex);
      alloc_owner[p] = choice;
    }
    // If this matches a registered struct type, co-allocate a stable
    // wasm scratch and record the translator.  alloc_mutex is released
    // before the wasm alloc and re-acquired briefly to publish the pair.
    size_t wasm_size = 0;
    meta_struct_translator_fn translator;
    {
      std::lock_guard<std::mutex> g(alloc_mutex);
      meta_struct_type_info const* match = nullptr;
      size_t match_count = 0;
      for (auto const& rec : struct_types) {
        if (rec.host_size == size) {
          ++match_count;
          match = &rec;
        }
      }
      if (match_count > 1) {
        std::fprintf(
          stderr,
          "rlbox_meta_sandbox: malloc(%zu) matches %zu registered struct "
          "types -- cannot disambiguate translator at alloc time.  "
          "Colliding symbols follow:\n",
          size, match_count);
        for (auto const& rec : struct_types) {
          if (rec.host_size == size) {
            std::fprintf(stderr, "  - %s\n", rec.symbol.c_str());
          }
        }
        std::abort();
      }
      if (match != nullptr) {
        wasm_size = match->wasm_size;
        translator = match->translator;
      }
    }
    if (wasm_size != 0) {
      auto scratch = wasm_sbx.impl_malloc_in_sandbox(wasm_size);
      if (scratch != 0) {
        std::lock_guard<std::mutex> g(alloc_mutex);
        struct_allocs[p] = meta_struct_alloc_info{ scratch,
                                                   std::move(translator) };
      }
    }
    return p;
  }

  inline void impl_free_in_sandbox(T_PointerType p)
  {
    if (p == 0) {
      return;
    }
    // alloc_owner is the source of truth, not the tag bit -- wasm
    // pointers can reach us untagged after going through
    // impl_get_sandboxed_pointer.  Look up both forms; fall back to the
    // tag only when the registry has no entry.
    T_PointerType registry_key = p;
    meta_backend owner;
    {
      std::lock_guard<std::mutex> g(alloc_mutex);
      auto it = alloc_owner.find(p);
      if (it == alloc_owner.end() && (p & META_TAG_MASK) == 0) {
        auto it2 = alloc_owner.find(tag_wasm(p));
        if (it2 != alloc_owner.end()) {
          registry_key = tag_wasm(p);
          owner = it2->second;
          alloc_owner.erase(it2);
        } else {
          owner = tag_owner(p);
        }
      } else if (it != alloc_owner.end()) {
        owner = it->second;
        alloc_owner.erase(it);
      } else {
        owner = tag_owner(p);
      }
    }
    T_PointerType raw = tag_strip(registry_key);
    uint32_t scratch_to_free = 0;
    {
      std::lock_guard<std::mutex> g(alloc_mutex);
      auto sit = struct_allocs.find(registry_key);
      if (sit != struct_allocs.end()) {
        scratch_to_free = sit->second.wasm_scratch;
        struct_allocs.erase(sit);
      }
    }
    if (scratch_to_free != 0) {
      wasm_sbx.impl_free_in_sandbox(scratch_to_free);
    }
    if (owner == meta_backend::wasm) {
      wasm_sbx.impl_free_in_sandbox(
        static_cast<rlbox_wasm2c_sandbox::T_PointerType>(raw));
    } else {
      process_sbx.impl_free_in_sandbox(raw);
    }
  }

  // Returns the owning backend of a top-level allocation, or nullopt for
  // interior / externally-derived pointers.  Not for general membership
  // queries -- use impl_is_pointer_in_sandbox_memory for that.
  std::pair<bool, meta_backend> owner_of(T_PointerType p)
  {
    std::lock_guard<std::mutex> g(alloc_mutex);
    auto it = alloc_owner.find(p);
    if (it != alloc_owner.end()) {
      return { true, it->second };
    }
    // Untagged wasm offsets reach us via impl_get_sandboxed_pointer; try
    // the tagged form so ownership-override still fires.
    if ((p & META_TAG_MASK) == 0) {
      auto it2 = alloc_owner.find(tag_wasm(p));
      if (it2 != alloc_owner.end()) {
        return { true, it2->second };
      }
    }
    return { false, meta_backend::process };
  }

  // Dispatch counters for tests/benchmarks.
  size_t mallocs_on_process() const { return mallocs_on_process_; }
  size_t mallocs_on_wasm() const { return mallocs_on_wasm_; }
  size_t invokes_on_process() const { return invokes_on_process_; }
  size_t invokes_on_wasm() const { return invokes_on_wasm_; }

  // Consecutive same-backend dispatches before a symbol pins; 0 disables.
  void set_pin_threshold(uint32_t n) { pin_threshold_ = n; }
  uint32_t pin_threshold() const { return pin_threshold_; }

  // Current pin state for func_name, or nullopt if not pinned/resolved.
  std::optional<meta_backend> pinned_backend_for(const char* func_name)
  {
    std::lock_guard<std::mutex> g(symbol_cache_mutex);
    auto it = symbol_cache.find(func_name);
    if (it == symbol_cache.end()) return std::nullopt;
    uint8_t p = it->second->pinned.load(std::memory_order_acquire);
    if (p == 0) return std::nullopt;
    return p == 1 ? meta_backend::process : meta_backend::wasm;
  }

  // Register a struct type for cross-ABI translation.  Each matching
  // allocation co-allocates a stable wasm scratch and pins the translator
  // to it; the translator runs on every wasm dispatch and a cleanup
  // closure mirrors state back.  Repeated registration of the same
  // symbol replaces; size collisions across distinct symbols abort.
  void register_struct_type(const char* symbol,
                            size_t host_size,
                            size_t wasm_size,
                            meta_struct_translator_fn translator)
  {
    std::lock_guard<std::mutex> g(alloc_mutex);
    for (auto& rec : struct_types) {
      if (rec.symbol == symbol) {
        rec.host_size = host_size;
        rec.wasm_size = wasm_size;
        rec.translator = std::move(translator);
        return;
      }
    }
    struct_types.push_back(meta_struct_type_info{
      symbol ? symbol : "", host_size, wasm_size, std::move(translator) });
  }
  // Stable wasm-scratch offset for a registered struct allocation, or 0.
  uint32_t get_struct_scratch(T_PointerType p)
  {
    std::lock_guard<std::mutex> g(alloc_mutex);
    auto it = struct_allocs.find(p);
    if (it == struct_allocs.end()) return 0;
    return it->second.wasm_scratch;
  }

  // Per-symbol latency snapshot for tests/introspection.  Policies should
  // read via meta_policy_context to avoid a hot-path lock acquire.
  struct meta_latency_snapshot
  {
    bool found = false;
    bool has_wasm = false;
    size_t proc_samples = 0;
    size_t wasm_samples = 0;
    double proc_median_ms = 0.0;
    double wasm_median_ms = 0.0;
  };
  meta_latency_snapshot latency_for(const char* func_name)
  {
    meta_latency_snapshot snap;
    std::unique_ptr<meta_symbol_record>* slot = nullptr;
    {
      std::lock_guard<std::mutex> g(symbol_cache_mutex);
      auto it = symbol_cache.find(func_name);
      if (it == symbol_cache.end()) {
        return snap;
      }
      slot = &it->second;
    }
    auto& rec = **slot;
    snap.found = true;
    snap.has_wasm = (rec.wasm_sym != nullptr);
    std::lock_guard<std::mutex> g(rec.stats_mutex);
    snap.proc_samples = rec.process_latency.size();
    snap.wasm_samples = rec.wasm_latency.size();
    snap.proc_median_ms = rec.process_latency.median();
    snap.wasm_median_ms = rec.wasm_latency.median();
    return snap;
  }

  template<typename T_Ret, typename... T_Args>
  inline T_PointerType impl_register_callback(void* key, void* callback)
  {
    // Register on both backends up front so either path can fire the
    // callback without a setup round-trip.  No rollback dance: wasm2c
    // aborts the process on slot exhaustion, killing it anyway.
    auto proc_ptr =
      process_sbx.impl_register_callback<T_Ret, T_Args...>(key, callback);
    if (proc_ptr == 0) {
      return 0;
    }
    auto wasm_slot =
      wasm_sbx.impl_register_callback<T_Ret, T_Args...>(key, callback);

    std::lock_guard<std::mutex> g(callback_mutex);
    callback_registry.emplace(proc_ptr, meta_callback_record{ wasm_slot, key });
    return proc_ptr;
  }

  template<typename T_Ret, typename... T_Args>
  inline void impl_unregister_callback(void* key)
  {
    // Drop the side-registry entry first (linear scan -- callbacks are
    // few), then tear down on both backends.
    {
      std::lock_guard<std::mutex> g(callback_mutex);
      for (auto it = callback_registry.begin(); it != callback_registry.end();
           ++it) {
        if (it->second.key == key) {
          callback_registry.erase(it);
          break;
        }
      }
    }
    process_sbx.impl_unregister_callback<T_Ret, T_Args...>(key);
    wasm_sbx.impl_unregister_callback<T_Ret, T_Args...>(key);
  }

  // Number of live registered callbacks.
  size_t callbacks_registered() const { return callback_registry.size(); }

  // Recover (meta*, user-key) when a registered callback fires.  Process
  // path: the process backend publishes its sandbox* on a dedicated
  // callback thread; map back through process_to_meta_map.  Wasm path:
  // synchronous on the invoker thread, so tl_invoking_meta is live.
  // Check process first -- if a wasm invoke is in flight on this thread
  // we still want to honor the actual process-side firer.
  static inline std::pair<rlbox_meta_sandbox*, void*>
  impl_get_executed_callback_sandbox_and_key()
  {
    auto proc = rlbox_process_sandbox::impl_get_executed_callback_sandbox_and_key();
    if (proc.first != nullptr) {
      std::lock_guard<std::mutex> g(meta_instance_mutex());
      auto& map = process_to_meta_map();
      auto it = map.find(proc.first);
      if (it != map.end()) {
        return { it->second, proc.second };
      }
    }
    if (tl_invoking_meta != nullptr) {
      auto wasm_pair =
        rlbox_wasm2c_sandbox::impl_get_executed_callback_sandbox_and_key();
      return { tl_invoking_meta, wasm_pair.second };
    }
    fputs("rlbox_meta_sandbox: impl_get_executed_callback_sandbox_and_key "
          "called outside an active backend callback (neither process TLS "
          "nor wasm invoke TLS is set).\n",
          stderr);
    std::abort();
  }
};

} // namespace rlbox

// Struct-translator macros: walk the library's existing
// sandbox_fields_reflection_<libId>_allClasses X-macro and emit one
// translator per struct plus a meta_<libId>_setup(meta&) registrar.
//
// Usage, in the embedder TU after rlbox_load_structs_from_library(<libId>):
//   rlbox_meta_load_struct_translators(<libId>);
// Then in setup code:
//   rlbox::meta_<libId>_setup(*sandbox.get_sandbox_impl());

// Field-copy expansion shared by forward and reverse walks.
#define rlbox_meta_copy_field_helper(TYPE, NAME, ATTR, ...)                    \
  dst->NAME = static_cast<decltype(dst->NAME)>(src->NAME);
#define rlbox_meta_nosep_helper()

// Emit one translator function inside namespace rlbox.
#define rlbox_meta_emit_one_struct_translator(T_Struct, libId)                 \
  inline meta_struct_translation meta_struct_translator_##libId##_##T_Struct(  \
    rlbox_meta_sandbox& meta, const char* /*symbol*/,                          \
    uintptr_t host_ptr_tagged)                                                 \
  {                                                                            \
    using HostLayout = Sbx_##libId##_##T_Struct<rlbox_process_sandbox>;        \
    using WasmLayout = Sbx_##libId##_##T_Struct<rlbox_wasm2c_sandbox>;         \
    void* host_va = meta.impl_get_unsandboxed_pointer<char>(host_ptr_tagged);  \
    auto* host_view = static_cast<HostLayout*>(host_va);                       \
    uint32_t wasm_offset = meta.get_struct_scratch(                            \
      static_cast<rlbox_meta_sandbox::T_PointerType>(host_ptr_tagged));        \
    if (wasm_offset == 0) {                                                    \
      std::fputs("meta_struct_translator_" #libId "_" #T_Struct                \
                 ": no scratch registered for allocation -- did you forget "    \
                 "meta_" #libId "_setup(meta)?\n",                             \
                 stderr);                                                      \
      std::abort();                                                            \
    }                                                                          \
    auto& wasm_sbx = meta.get_wasm_sbx();                                      \
    auto* wasm_view = static_cast<WasmLayout*>(                                \
      wasm_sbx.impl_get_unsandboxed_pointer<char>(wasm_offset));               \
    {                                                                          \
      auto* src = host_view;                                                   \
      auto* dst = wasm_view;                                                   \
      sandbox_fields_reflection_##libId##_class_##T_Struct(                    \
        rlbox_meta_copy_field_helper, rlbox_meta_nosep_helper)                 \
    }                                                                          \
    meta_struct_translation out;                                               \
    out.wasm_scratch = rlbox_meta_sandbox::tag_wasm(                           \
      static_cast<rlbox_meta_sandbox::T_PointerType>(wasm_offset));            \
    out.cleanup = [&meta, host_view, wasm_offset]() {                          \
      auto& ws = meta.get_wasm_sbx();                                          \
      auto* wv = static_cast<WasmLayout*>(                                     \
        ws.impl_get_unsandboxed_pointer<char>(wasm_offset));                   \
      auto* src = wv;                                                          \
      auto* dst = host_view;                                                   \
      sandbox_fields_reflection_##libId##_class_##T_Struct(                    \
        rlbox_meta_copy_field_helper, rlbox_meta_nosep_helper)                 \
    };                                                                         \
    return out;                                                                \
  }

// Register one struct's translator with the meta from inside meta_<libId>_setup.
#define rlbox_meta_register_one_struct(T_Struct, libId)                        \
  meta.register_struct_type(                                                   \
    #libId "::" #T_Struct,                                                     \
    sizeof(Sbx_##libId##_##T_Struct<rlbox_process_sandbox>),                   \
    sizeof(Sbx_##libId##_##T_Struct<rlbox_wasm2c_sandbox>),                    \
    meta_struct_translator_##libId##_##T_Struct);

// Emit all translators + the setup function for one library.
#define rlbox_meta_load_struct_translators(libId)                              \
  namespace rlbox {                                                            \
  sandbox_fields_reflection_##libId##_allClasses(                              \
    rlbox_meta_emit_one_struct_translator)                                     \
  inline void meta_##libId##_setup(rlbox_meta_sandbox& meta)                   \
  {                                                                            \
    sandbox_fields_reflection_##libId##_allClasses(                            \
      rlbox_meta_register_one_struct)                                          \
  }                                                                            \
  }                                                                            \
  static_assert(true, "require semicolon after rlbox_meta_load_struct_translators")

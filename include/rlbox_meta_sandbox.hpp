#pragma once

// Meta-sandbox: a T_Sbx that composes the process and wasm2c backends and
// dispatches each invoke through a policy hook.
//
// Both backends share a single memfd: the process backend mints it, and the
// meta hands the same region to wasm2c as its linear memory (see
// rlbox_meta_wasm2c_heap.h).  Host and shim map it at the same VA, so
// T_PointerType is uniformly a host VA in that region -- no tag bits, no
// owner registry, no per-pointer translation.  Allocator topology partitions
// the region by offset (wasm low, process mspace high), so free routes off
// the offset alone.
//
// Per-symbol latency rings let the policy hook route by observed cost; an
// adaptive policy is provided.  Symbols are resolved dynamically per backend
// at dispatch time.
//
// Before including, define RLBOX_WASM2C_MODULE_NAME and include the generated
// wasm2c header.  Do NOT define RLBOX_USE_STATIC_CALLS().

#include <algorithm>
#include <array>
#include <atomic>
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

#include "rlbox_meta_wasm2c_heap.h"
#include "rlbox_process_sandbox.hpp"
#include "rlbox_wasm2c_sandbox.hpp"

#ifndef RLBOX_WASM2C_MODULE_NAME
#  error "rlbox_meta_sandbox.hpp: define RLBOX_WASM2C_MODULE_NAME before include"
#endif

#define RLBOX_META_STRINGIFY_INNER(x) #x
#define RLBOX_META_STRINGIFY(x) RLBOX_META_STRINGIFY_INNER(x)
// Must match RLBOX_WASM2C_MODULE_FUNC in rlbox_wasm2c_sandbox.hpp.
#define RLBOX_META_WASM_PREFIX                                                 \
  "w2c_0x24" RLBOX_META_STRINGIFY(RLBOX_WASM2C_MODULE_NAME) "0x2Ewasm_"

namespace rlbox {

// Which underlying backend handles a given invoke.
enum class meta_backend
{
  process,
  wasm,
};

// Per-call context the policy hook sees. func_name is nullptr on the
// allocation path; latency fields are populated as a snapshot.
struct meta_policy_context
{
  const char* func_name;  // resolved symbol name (nullptr for allocation path)
  bool has_wasm = false;  // true iff the symbol resolves on the wasm side
  size_t proc_samples = 0;
  size_t wasm_samples = 0;
  double proc_median_ms = 0.0;
  double wasm_median_ms = 0.0;

  // Cumulative invokes of this symbol (zero on alloc path). Survives past
  // the latency ring's saturation point so it can drive re-exploration on
  // periods larger than the ring size.
  size_t invoke_count = 0;

  // alloc_size is bytes requested on the alloc path (zero on invoke path);
  // proc_median_sum/wasm_median_sum sum per-symbol medians across symbols
  // that resolve on both backends with at least one sample per side.
  size_t alloc_size = 0;
  double proc_median_sum = 0.0;
  double wasm_median_sum = 0.0;
};

using meta_policy_fn = std::function<meta_backend(const meta_policy_context&)>;

class rlbox_meta_sandbox;

// Returned by a struct translator on the wasm dispatch path. wasm_scratch
// is the tagged T_PointerType to pass to wasm in place of the host-layout
// arg; cleanup is an RAII closure that syncs wasm->host after the invoke.
struct meta_struct_translation
{
  uintptr_t wasm_scratch;
  std::function<void()> cleanup;
};

// Per-arg translator hook used on wasm dispatch.
using meta_struct_translator_fn = std::function<meta_struct_translation(
  rlbox_meta_sandbox& meta,
  const char* symbol,
  uintptr_t host_ptr)>;

// Adaptive policy with a global `current_best`. Every invoke dispatches to
// it then updates it; alloc invokes follow without updating. Update rule:
//   - first `explore` invocations -> random (seed both backends).
//   - reexplore_period != 0 and on the Nth boundary -> random.
//   - tie (including both-zero) -> random.
//   - else lower median sum wins.
// current_best=wasm + symbol with no wasm binding -> fall back to process.
inline meta_policy_fn make_adaptive_global_policy(size_t explore = 1,
                                                  size_t reexplore_period = 0)
{
  struct adaptive_state
  {
    meta_backend current_best = meta_backend::process;
    size_t named_invoke_count = 0;
    std::mt19937 rng{ std::random_device{}() };
    std::uniform_int_distribution<int> dist{ 0, 1 };
  };
  auto state = std::make_shared<adaptive_state>();
  return [explore, reexplore_period, state](
           const meta_policy_context& ctx) -> meta_backend {
    if (ctx.func_name == nullptr) {
      return state->current_best;
    }

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
    if (n < explore) {
      next = random_backend();
    } else if (reexplore_period != 0 &&
               ((n - explore) % reexplore_period) == 0) {
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

// Adaptive policy that decides per symbol from its own latency rings. Per
// invoke (named):
//   - !has_wasm -> process.
//   - both rings under `explore` -> random.
//   - one ring under `explore` -> route to the under-sampled side.
//   - reexplore_period != 0 and (invoke_count % period) == 0 -> probe loser.
//   - else lower median wins; tie -> random.
// Alloc path alternates process/wasm so both shared-memory partitions stay
// exercised (they back the same memfd, so allocator choice is otherwise
// indistinguishable to consumers).
inline meta_policy_fn make_adaptive_per_function_policy(
  size_t explore = 3,
  size_t reexplore_period = 0)
{
  struct per_function_state
  {
    bool alloc_toggle = false;
    std::mt19937 rng{ std::random_device{}() };
    std::uniform_int_distribution<int> dist{ 0, 1 };
  };
  auto state = std::make_shared<per_function_state>();
  return [explore, reexplore_period, state](
           const meta_policy_context& ctx) -> meta_backend {
    auto random_backend = [&]() {
      return state->dist(state->rng) == 0 ? meta_backend::process
                                          : meta_backend::wasm;
    };

    if (ctx.func_name == nullptr) {
      bool pick_wasm = state->alloc_toggle;
      state->alloc_toggle = !state->alloc_toggle;
      return pick_wasm ? meta_backend::wasm : meta_backend::process;
    }

    if (!ctx.has_wasm) {
      return meta_backend::process;
    }

    bool proc_under = ctx.proc_samples < explore;
    bool wasm_under = ctx.wasm_samples < explore;
    if (proc_under && wasm_under) {
      return random_backend();
    }
    if (proc_under) {
      return meta_backend::process;
    }
    if (wasm_under) {
      return meta_backend::wasm;
    }

    if (reexplore_period != 0 && ctx.invoke_count != 0 &&
        (ctx.invoke_count % reexplore_period) == 0) {
      if (ctx.proc_median_ms > ctx.wasm_median_ms) {
        return meta_backend::process;
      }
      if (ctx.wasm_median_ms > ctx.proc_median_ms) {
        return meta_backend::wasm;
      }
      return random_backend();
    }

    if (ctx.proc_median_ms < ctx.wasm_median_ms) {
      return meta_backend::process;
    }
    if (ctx.wasm_median_ms < ctx.proc_median_ms) {
      return meta_backend::wasm;
    }
    return random_backend();
  };
}

// Default factory. Compile with -DRLBOX_META_ADAPTIVE_PER_FUNCTION=1 (set via
// the CMake RLBOX_META_ADAPTIVE_MODE option) to get per-function selection;
// otherwise falls back to the global current_best behavior. Embedders that
// want explicit control can call the named factories directly.
inline meta_policy_fn make_adaptive_policy(size_t explore = 1,
                                           size_t reexplore_period = 0)
{
#ifdef RLBOX_META_ADAPTIVE_PER_FUNCTION
  return make_adaptive_per_function_policy(explore, reexplore_period);
#else
  return make_adaptive_global_policy(explore, reexplore_period);
#endif
}

// Return-type extractor for a raw C function type.
template<typename F> struct meta_fn_ret;
template<typename R, typename... A> struct meta_fn_ret<R(A...)> { using type = R; };

class rlbox_meta_sandbox
{
public:
  // Integer widths follow the process backend (host-native 64-bit); wasm
  // dispatch narrows at the boundary.
  using T_LongLongType = rlbox_process_sandbox::T_LongLongType;
  using T_LongType = rlbox_process_sandbox::T_LongType;
  using T_IntType = rlbox_process_sandbox::T_IntType;
  using T_PointerType = rlbox_process_sandbox::T_PointerType;
  using T_ShortType = rlbox_process_sandbox::T_ShortType;

  // T_PointerType values are host VAs into the shared memfd. Wasm-allocated
  // offsets are widened to (heap_base + offset) at the meta boundary; the
  // narrow back to uint32_t at the wasm dispatch site is implicit because
  // heap_base is 4-GiB-aligned. Free routing partitions the offset:
  // < RLBOX_SHM_PROCESS_OFFSET = wasm allocator, >= = process mspace.

protected:
  rlbox_process_sandbox process_sbx;
  rlbox_wasm2c_sandbox wasm_sbx;

public:
  // Exposed for struct-translator hooks living in the embedder TU.
  rlbox_process_sandbox& get_process_sbx() { return process_sbx; }
  rlbox_wasm2c_sandbox& get_wasm_sbx() { return wasm_sbx; }

protected:

  static double monotonic_ms()
  {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
  }

  // Fixed-size ring of recent latencies (ms). Small on purpose so the
  // median tracks current steady state rather than lifetime average.
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

  // One record per looked-up symbol. Returned as the opaque void* from
  // impl_lookup_symbol; lifetime tied to this sandbox.
  struct meta_symbol_record
  {
    std::string name;
    void* process_sym = nullptr;
    void* wasm_sym = nullptr;
    mutable std::mutex stats_mutex;
    latency_ring process_latency;
    latency_ring wasm_latency;

    // Symbol pinning fast path. After `pin_threshold_` consecutive
    // dispatches to the same backend, `pinned` flips to 1=process / 2=wasm
    // and subsequent invokes skip policy/ownership/sample-push.
    std::atomic<uint8_t> pinned{0};
    uint32_t consecutive_same_picks = 0;
    meta_backend last_pick = meta_backend::process;
  };

  std::mutex symbol_cache_mutex;
  std::unordered_map<std::string, std::unique_ptr<meta_symbol_record>>
    symbol_cache;

  // Struct-ABI translation registry. Host (64-bit) and wasm (32-bit) struct
  // layouts differ even under shared memory, so each registered host alloc
  // gets a co-allocated wasm-layout scratch; translators copy fields across
  // on every wasm dispatch. Keyed by host_size -- two distinct symbols with
  // the same host_size abort at alloc time (rlbox's malloc API surfaces only
  // the size, so we cannot pick a translator unambiguously).
  std::mutex alloc_mutex;
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

  // Dispatch counters for tests and benchmarks. Reflect post-override
  // (ownership-wins) choice, not the raw policy pick.
  size_t mallocs_on_process_ = 0;
  size_t mallocs_on_wasm_ = 0;
  size_t invokes_on_process_ = 0;
  size_t invokes_on_wasm_ = 0;

  // Eager dual callback registration. The host function is installed on
  // both backends; the process trampoline VA serves as the stable
  // meta-level handle, with wasm-dispatch rewriting it to the paired slot.
  struct meta_callback_record
  {
    rlbox_wasm2c_sandbox::T_PointerType wasm_slot;
    void* key;
  };
  std::mutex callback_mutex;
  std::unordered_map<T_PointerType, meta_callback_record> callback_registry;

  meta_policy_fn policy;

  // After this many consecutive slow-path dispatches to the same backend,
  // the symbol pins and subsequent invokes take the lock-free fast path.
  // Set 0 to disable. Once pinned, the policy is no longer consulted, so
  // adaptive's `reexplore_period` won't fire on that symbol.
  uint32_t pin_threshold_ = 16;

public:
  rlbox_meta_sandbox()
    : policy([](const meta_policy_context&) { return meta_backend::process; })
  {}

  // Override the per-call dispatch policy. Called on every invoke hot path.
  void set_policy(meta_policy_fn new_policy)
  {
    policy = std::move(new_policy);
  }

  // Static registry of live meta instances, keyed by the underlying
  // process sandbox. Process callbacks dispatch on a dedicated host
  // thread, so per-thread TLS isn't visible to them; this map lets us
  // recover the owning meta when a process callback fires.
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

  // TLS for the wasm-invoke callback path. Set on wasm-branch entry and
  // cleared via RAII; a wasm-side callback fired mid-invoke reads this to
  // identify the meta. `inline` so multiple TUs don't collide on linkage.
  static inline thread_local rlbox_meta_sandbox* tl_invoking_meta = nullptr;

  // Process comes up first so we can hand its shared memfd to wasm as
  // its linear memory; wasm allocates in [0..RLBOX_SHM_PROCESS_OFFSET),
  // process mspace owns the rest. If either side fails we tear the other
  // back down.
  template<typename T_Char>
  inline bool impl_create_sandbox(const T_Char* library_path)
  {
    if (!process_sbx.impl_create_sandbox(library_path)) {
      return false;
    }
    {
      void* shared_base = process_sbx.impl_get_memory_location();
      uint32_t wasm_pages =
        (uint32_t)(RLBOX_SHM_PROCESS_OFFSET / 65536u);
      rlbox_meta_set_pending_shared_heap(shared_base, wasm_pages, wasm_pages);
    }
    if (!wasm_sbx.impl_create_sandbox()) {
      rlbox_meta_set_pending_shared_heap(nullptr, 0, 0);
      process_sbx.impl_destroy_sandbox();
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
    return reinterpret_cast<void*>(p);
  }

  template<typename T>
  inline T_PointerType impl_get_sandboxed_pointer(const void* p) const
  {
    return rlbox_process_sandbox::host_ptr_to_sbx(p);
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

  // 3-arg form so rlbox hands us `expensive_sandbox_finder`. Two pointers
  // owned by the same meta still need backend disambiguation, since the
  // process backend's 2-arg form would say "same" for a (host, wasm) pair.
  static inline bool impl_is_in_same_sandbox(
    const void* p1,
    const void* p2,
    rlbox_meta_sandbox* (*expensive_sandbox_finder)(
      const void* example_sandbox_ptr))
  {
    // find_sandbox_from_example aborts on null; guard for null tolerance.
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

  // Resolve the symbol in both backends and return a stable handle. Wasm
  // lookup may return nullptr for wrapper-only symbols; a policy that
  // routes to wasm for such a name aborts in impl_invoke_with_func_ptr.
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
    // RLBOX_USE_STATIC_CALLS on, which the meta cannot use). Resolve the
    // generated thunk directly via dlsym; the host binary needs -rdynamic.
    std::string wasm_name = RLBOX_META_WASM_PREFIX;
    wasm_name += func_name;
    rec->wasm_sym = dlsym(RTLD_DEFAULT, wasm_name.c_str());
    auto* raw = rec.get();
    symbol_cache.emplace(rec->name, std::move(rec));
    return raw;
  }

  // Wasm-path arg rewrite: swap a registered callback handle for its
  // paired wasm slot index. Non-callback values pass through unchanged.
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

    // Fast path for pinned symbols. Skips policy/sampling but keeps the
    // correctness-critical struct translation, callback rewrite, and
    // tl_invoking_meta RAII. Set pin_threshold_=0 to disable.
    uint8_t pin = rec->pinned.load(std::memory_order_acquire);
    if (pin == 2) {
      invokes_on_wasm_++;
      std::vector<std::function<void()>> struct_cleanups;
      const char* sym_name = rec->name.c_str();
      auto translate = [&](auto&& a) {
        using A = std::remove_cv_t<std::remove_reference_t<decltype(a)>>;
        if constexpr (std::is_same_v<A, T_PointerType>) {
          A val = static_cast<A>(a);
          if (val == 0) {
            return val;
          }
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
            return static_cast<A>(narrow_to_wasm_offset(t.wasm_scratch));
          }
          {
            std::lock_guard<std::mutex> g(callback_mutex);
            auto it = callback_registry.find(val);
            if (it != callback_registry.end()) {
              return static_cast<A>(it->second.wasm_slot);
            }
          }
          // Only narrow if val is actually a host VA in the shared region.
          // T_PointerType is uintptr_t which on LP64 also matches scalar
          // uLong args (e.g. adler32's running checksum), so we can't
          // unconditionally subtract heap_base; that would corrupt scalars.
          auto base = rlbox_process_sandbox::host_ptr_to_sbx(
            process_sbx.impl_get_memory_location());
          if (val >= base && val < base + RLBOX_SHM_REGION_BYTES) {
            return static_cast<A>(narrow_to_wasm_offset(val));
          }
          return val;
        } else {
          return static_cast<A>(std::forward<decltype(a)>(a));
        }
      };
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
      struct meta_scope
      {
        rlbox_meta_sandbox* prev;
        ~meta_scope() { tl_invoking_meta = prev; }
      };
      meta_scope ms{ tl_invoking_meta };
      tl_invoking_meta = this;
      cleanup_runner cr{ &struct_cleanups };
      using T_Ret_pin2 = typename meta_fn_ret<T>::type;
      if constexpr (std::is_pointer_v<T_Ret_pin2>) {
        auto raw = wasm_sbx.impl_invoke_with_func_ptr<T, T_Converted>(
          reinterpret_cast<T_Converted*>(rec->wasm_sym),
          translate(std::forward<T_Args>(params))...);
        if (raw != 0) {
          return widen_wasm_offset(static_cast<uint32_t>(raw));
        }
        return static_cast<T_PointerType>(0);
      } else {
        return wasm_sbx.impl_invoke_with_func_ptr<T, T_Converted>(
          reinterpret_cast<T_Converted*>(rec->wasm_sym),
          translate(std::forward<T_Args>(params))...);
      }
    }
    if (pin == 1) {
      invokes_on_process_++;
      return process_sbx.impl_invoke_with_func_ptr<T, T_Converted>(
        reinterpret_cast<T_Converted*>(rec->process_sym),
        std::forward<T_Args>(params)...);
    }

    // Slow path: policy + timing. After enough consecutive same-backend
    // picks, sample_pusher flips rec->pinned.
    meta_policy_context ctx{};
    ctx.func_name = rec->name.c_str();
    ctx.has_wasm = (rec->wasm_sym != nullptr);
    {
      std::lock_guard<std::mutex> g(rec->stats_mutex);
      ctx.proc_samples = rec->process_latency.size();
      ctx.wasm_samples = rec->wasm_latency.size();
      ctx.proc_median_ms = rec->process_latency.median();
      ctx.wasm_median_ms = rec->wasm_latency.median();
      ctx.invoke_count =
        rec->process_latency.writes + rec->wasm_latency.writes;
    }
    snapshot_median_sums(ctx.proc_median_sum, ctx.wasm_median_sum);
    meta_backend choice = policy(ctx);

    // RAII sample pusher: times the forwarded call and updates the pin
    // counter under the same stats_mutex hold.
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
      // Per-arg translate for the wasm path: registered struct allocs run
      // their translator; registered callback handles map to slot indices;
      // raw host VAs in the shared region narrow to wasm offsets explicitly
      // so heap_base alignment isn't required. Scalars sharing T_PointerType
      // (uLong on LP64) fall through; the implicit truncation handles them.
      std::vector<std::function<void()>> struct_cleanups;
      const char* sym_name = rec->name.c_str();
      auto translate = [&](auto&& a) {
        using A = std::remove_cv_t<std::remove_reference_t<decltype(a)>>;
        if constexpr (std::is_same_v<A, T_PointerType>) {
          A val = static_cast<A>(a);
          if (val == 0) {
            return val;
          }
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
            return static_cast<A>(narrow_to_wasm_offset(t.wasm_scratch));
          }
          {
            std::lock_guard<std::mutex> g(callback_mutex);
            auto it = callback_registry.find(val);
            if (it != callback_registry.end()) {
              return static_cast<A>(it->second.wasm_slot);
            }
          }
          // See pin==2 fast path for why we range-check before narrowing.
          auto base = rlbox_process_sandbox::host_ptr_to_sbx(
            process_sbx.impl_get_memory_location());
          if (val >= base && val < base + RLBOX_SHM_REGION_BYTES) {
            return static_cast<A>(narrow_to_wasm_offset(val));
          }
          return val;
        } else {
          return static_cast<A>(std::forward<decltype(a)>(a));
        }
      };

      // Order matters: sp is destroyed after cleanup_runner (reverse
      // decl order) so the timed sample covers translation cost too.
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
      // RAII-stash the invoking meta so a wasm-fired host callback can
      // recover it via impl_get_executed_callback_sandbox_and_key.
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
      using T_Ret_wasm = typename meta_fn_ret<T>::type;
      if constexpr (std::is_pointer_v<T_Ret_wasm>) {
        auto raw = wasm_sbx.impl_invoke_with_func_ptr<T, T_Converted>(
          reinterpret_cast<T_Converted*>(rec->wasm_sym),
          translate(std::forward<T_Args>(params))...);
        if (raw != 0) {
          return widen_wasm_offset(static_cast<uint32_t>(raw));
        }
        return static_cast<T_PointerType>(0);
      } else {
        return wasm_sbx.impl_invoke_with_func_ptr<T, T_Converted>(
          reinterpret_cast<T_Converted*>(rec->wasm_sym),
          translate(std::forward<T_Args>(params))...);
      }
    }

    invokes_on_process_++;
    sample_pusher sp{ rec, meta_backend::process, monotonic_ms(),
                      pin_threshold_ };
    return process_sbx.impl_invoke_with_func_ptr<T, T_Converted>(
      reinterpret_cast<T_Converted*>(rec->process_sym),
      std::forward<T_Args>(params)...);
  }

  // Widen a wasm offset to a host VA in the shared region, or convert
  // back. The wasm heap_base equals process_sbx's memfd base (set up by
  // the heap-injection wrap), so a single addition / subtraction round-trips.
  // Narrow uses explicit subtraction (not implicit truncation) so the host
  // mapping doesn't have to be 4-GiB-aligned.
  inline T_PointerType widen_wasm_offset(uint32_t offset) const
  {
    return rlbox_process_sandbox::host_ptr_to_sbx(
             process_sbx.impl_get_memory_location()) +
           static_cast<T_PointerType>(offset);
  }
  inline uint32_t narrow_to_wasm_offset(T_PointerType host_va) const
  {
    auto base = rlbox_process_sandbox::host_ptr_to_sbx(
      process_sbx.impl_get_memory_location());
    return static_cast<uint32_t>(host_va - base);
  }

  // Snapshot per-backend median sums across symbols that resolve on both
  // backends with >=1 sample per side. We release symbol_cache_mutex before
  // acquiring each record's stats_mutex to avoid nesting locks; records
  // live for the sandbox's lifetime, so raw pointers stay valid.
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
    // Policy is called with func_name == nullptr to indicate the alloc
    // path. Snapshot the aggregate latency view so adaptive policies can
    // route allocations toward the currently-faster backend.
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
      p = widen_wasm_offset(raw);
      mallocs_on_wasm_++;
    } else {
      p = process_sbx.impl_malloc_in_sandbox(size);
      if (p == 0) {
        return 0;
      }
      mallocs_on_process_++;
    }

    // If this matches a registered struct type, co-allocate a stable
    // wasm scratch. Release alloc_mutex between the lookup and the
    // backend call; re-acquire briefly to publish the pair.
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
    uint32_t scratch_to_free = 0;
    {
      std::lock_guard<std::mutex> g(alloc_mutex);
      auto sit = struct_allocs.find(p);
      if (sit != struct_allocs.end()) {
        scratch_to_free = sit->second.wasm_scratch;
        struct_allocs.erase(sit);
      }
    }
    if (scratch_to_free != 0) {
      wasm_sbx.impl_free_in_sandbox(scratch_to_free);
    }
    // Allocator partitioning encodes ownership in the offset: low partition
    // (< RLBOX_SHM_PROCESS_OFFSET) was minted by the wasm allocator; the
    // high partition is the process mspace.
    uint32_t offset = narrow_to_wasm_offset(p);
    if (offset < RLBOX_SHM_PROCESS_OFFSET) {
      wasm_sbx.impl_free_in_sandbox(
        static_cast<rlbox_wasm2c_sandbox::T_PointerType>(offset));
    } else {
      process_sbx.impl_free_in_sandbox(p);
    }
  }

  // Dispatch counters for tests / benchmarks.
  size_t mallocs_on_process() const { return mallocs_on_process_; }
  size_t mallocs_on_wasm() const { return mallocs_on_wasm_; }
  size_t invokes_on_process() const { return invokes_on_process_; }
  size_t invokes_on_wasm() const { return invokes_on_wasm_; }

  // Consecutive same-backend dispatches before a symbol pins. 0 disables.
  void set_pin_threshold(uint32_t n) { pin_threshold_ = n; }
  uint32_t pin_threshold() const { return pin_threshold_; }

  // Pin state for `func_name`, or nullopt if not pinned / not looked up.
  std::optional<meta_backend> pinned_backend_for(const char* func_name)
  {
    std::lock_guard<std::mutex> g(symbol_cache_mutex);
    auto it = symbol_cache.find(func_name);
    if (it == symbol_cache.end()) return std::nullopt;
    uint8_t p = it->second->pinned.load(std::memory_order_acquire);
    if (p == 0) return std::nullopt;
    return p == 1 ? meta_backend::process : meta_backend::wasm;
  }

  // Register a struct for meta->wasm ABI translation. Each matching alloc
  // co-allocates a wasm-layout scratch and pins this translator to it; on
  // wasm dispatch the translator copies host->wasm and returns a cleanup
  // closure that mirrors back. Re-registering `symbol` replaces; two
  // distinct symbols with the same host_size aborts at alloc.
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

  // Per-symbol latency snapshot for tests and introspection. Policies
  // should read via meta_policy_context to avoid the extra lock.
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
    // Register on both backends so either dispatch path can fire the
    // callback. If wasm aborts (slot exhaustion), the host process is
    // already dead, so we don't bother with partial rollback.
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
    // Drop the side-registry entry first (linear scan by key since the
    // registry is keyed by proc_ptr), then tear down on both backends.
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

  // Number of live registered callbacks. Used by tests to verify dual
  // registration without depending on backend internals.
  size_t callbacks_registered() const { return callback_registry.size(); }

  // Recover (meta, user-key) when a host callback fires. Process callbacks
  // run on the backend's dedicated thread (no shared TLS), so we map
  // process_sbx* -> meta via the static registry. Wasm callbacks dispatch
  // synchronously and read tl_invoking_meta. Check process first or a wasm
  // invoke in flight on this thread would mask a process-side firer.
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

// Struct-translator macros. Walk the existing
// sandbox_fields_reflection_<libId>_allClasses X-macro to emit one
// translator per struct plus a meta_<libId>_setup(meta&) registration
// function. Use after rlbox_load_structs_from_library(<libId>):
//
//   rlbox_meta_load_struct_translators(<libId>);
//
// then call rlbox::meta_<libId>_setup(*sandbox.get_sandbox_impl()).

// Field-copy helper used in both directions. Inspects the source-language
// type from the X-macro: pointers narrow/widen via heap_base arithmetic,
// scalars use plain static_cast. Implicit truncation isn't enough since
// heap_base isn't required to be 4-GiB-aligned.
#define rlbox_meta_copy_field_helper(TYPE, NAME, ATTR, ...)                    \
  rlbox::rlbox_meta_copy_field<TYPE>(dst->NAME, src->NAME, meta);
#define rlbox_meta_nosep_helper()

namespace rlbox {

template<typename FieldType, typename Dst, typename Src>
inline void rlbox_meta_copy_field(Dst& dst,
                                  const Src& src,
                                  rlbox_meta_sandbox& meta)
{
  if constexpr (std::is_pointer_v<FieldType>) {
    if constexpr (sizeof(Dst) <= 4 && sizeof(Src) > 4) {
      // host -> wasm: convert host VA to wasm offset.
      auto v = static_cast<uintptr_t>(src);
      dst = (v == 0) ? 0
                     : static_cast<Dst>(meta.narrow_to_wasm_offset(v));
    } else if constexpr (sizeof(Dst) > 4 && sizeof(Src) <= 4) {
      // wasm -> host: widen offset back to host VA.
      auto v = static_cast<uint32_t>(src);
      dst = (v == 0) ? static_cast<Dst>(0)
                     : static_cast<Dst>(meta.widen_wasm_offset(v));
    } else {
      // Same-width pointer (e.g. host==wasm bitness): plain copy.
      dst = static_cast<Dst>(src);
    }
  } else {
    dst = static_cast<Dst>(src);
  }
}

} // namespace rlbox

// Emit one translator. Generated symbol lives in namespace rlbox so the
// Sbx_<...> references resolve without qualification.
#define rlbox_meta_emit_one_struct_translator(T_Struct, libId)                 \
  inline meta_struct_translation meta_struct_translator_##libId##_##T_Struct(  \
    rlbox_meta_sandbox& meta, const char* /*symbol*/,                          \
    uintptr_t host_ptr)                                                        \
  {                                                                            \
    using HostLayout = Sbx_##libId##_##T_Struct<rlbox_process_sandbox>;        \
    using WasmLayout = Sbx_##libId##_##T_Struct<rlbox_wasm2c_sandbox>;         \
    auto* host_view = reinterpret_cast<HostLayout*>(host_ptr);                 \
    uint32_t wasm_offset = meta.get_struct_scratch(                            \
      static_cast<rlbox_meta_sandbox::T_PointerType>(host_ptr));               \
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
    out.wasm_scratch = meta.widen_wasm_offset(wasm_offset);                    \
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

// Register one struct's translator with the meta.
#define rlbox_meta_register_one_struct(T_Struct, libId)                        \
  meta.register_struct_type(                                                   \
    #libId "::" #T_Struct,                                                     \
    sizeof(Sbx_##libId##_##T_Struct<rlbox_process_sandbox>),                   \
    sizeof(Sbx_##libId##_##T_Struct<rlbox_wasm2c_sandbox>),                    \
    meta_struct_translator_##libId##_##T_Struct);

// Top-level: emit all translators + the setup function. Call once per
// library at namespace scope, after rlbox_load_structs_from_library.
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

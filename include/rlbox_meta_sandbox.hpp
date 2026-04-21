#pragma once

// Meta-sandbox: a T_Sbx that composes the process and wasm2c backends and
// dispatches each invoke through a policy hook.  M3 introduced per-call
// policy dispatch with per-backend symbol resolution; M4 added per-
// allocation backend pinning (via an owner registry that overrides the
// policy when a pointer-typed arg is owned by a backend other than the
// one the policy picked).  M5 makes wasm-side allocations actually work
// by encoding the owning backend in bit 63 of T_PointerType — process
// VAs on x86_64 never set bit 63 in user space, and wasm offsets are
// 32-bit, so the top bit is free on both sides.  impl_malloc_in_sandbox
// now consults the policy (with func_name == nullptr signaling an
// allocation context) to decide which backend to allocate from; wasm
// allocations are widened to uintptr_t and OR'd with the tag bit on the
// way out.  impl_free_in_sandbox reads the tag bit to route to the
// correct backend.  impl_get_unsandboxed_pointer is tag-aware so
// dereferences of wasm-owned tainted pointers land in wasm linear
// memory.  impl_invoke_with_func_ptr still consults the alloc registry
// for ownership override (tag+registry are consistent: registry keys are
// the full tagged T_PointerType values, so there's a single source of
// truth).  When forwarding a wasm invoke, the C++ narrowing from
// uintptr_t to wasm's uint32_t T_PointerType strips the tag for free.
// M6 adds eager dual callback registration: impl_register_callback
// registers the host function on *both* backends and stashes the
// pairing in a side registry.  The process backend's trampoline address
// is returned as the stable meta-level handle; on wasm-dispatch we
// rewrite any arg matching a handle to the paired wasm slot index
// before forwarding.  Kept separate from alloc_owner because a callback
// registered on both backends has no single owner and shouldn't trip
// the ownership-override path.
// M7 adds rolling per-(symbol, backend) latency history: every invoke
// is timed end-to-end around the forwarded call, with the sample
// pushed into a fixed-size ring inside the symbol's meta_symbol_record.
// The policy hook sees a snapshot of both backends' sample counts and
// medians through meta_policy_context, so adaptive policies can route
// by observed cost.  A make_adaptive_policy() helper packages the
// expected pattern (explore each backend N times per symbol, then
// route to whichever median is lower).  Ownership override still wins
// over adaptive routing for the same reason it wins over any other
// policy pick.
// M8 extends meta_policy_context with an aggregate view for the alloc
// path.  impl_malloc_in_sandbox snapshots the symbol cache and counts,
// across all symbols that resolve on both sides, how many have a
// lower median on process vs wasm ("wins").  make_adaptive_policy
// routes allocations to the winning side, falling back to process
// when there's no head-to-head signal yet.  The point is to break the
// pre-M8 chain "alloc always on process → every pointer-carrying
// invoke pinned to process via ownership-wins → wasm dispatch never
// fires for libraries that allocate": once wasm is consistently
// winning symbol-level races, new allocations follow, and the
// ownership-override then pins consumers to wasm automatically.
// Alloc-time routing is a guess (we don't know the consumer symbol
// yet), so the signal is conservative — tie goes to process.
// M9 adds per-dispatch struct ABI translation.  rlbox emits struct
// layouts parameterised on T_Sbx; meta inherits process's 64-bit
// T_PointerType, so a meta-allocated z_stream has host-ABI offsets
// (8-byte pointers, padded uLongs).  Wasm-side compiled code reads
// the same struct at its 32-bit ABI.  The mismatch is now handled at
// the invoke boundary: the embedder registers one or more struct
// sizes with register_struct_size, and whenever a T_PointerType arg
// pointing at an allocation of that size is about to cross into
// wasm, the registered struct_translator runs — allocates a wasm
// scratch in wasm linear memory, copies fields host→wasm with
// per-field width adjustment, and hands the scratch pointer to the
// wasm invoke.  An RAII cleanup then copies fields wasm→host and
// frees the scratch so subsequent host-side field reads observe
// post-invoke state (avail_in, total_out, adler, etc.).  The
// translator lives in the embedder TU (main_meta.cpp for zlib)
// because it references Sbx_<lib>_<struct><T_Sbx>, which is only
// emitted by rlbox_load_structs_from_library and is unknown inside
// this header.  Zero overhead on the process path (we guard on
// struct_translator + alloc_size match).
// Default policy still routes every call to process, so byte-identical
// zlib behavior is preserved.  See CLAUDE.md ("Dynamic sandbox
// selection") for the overall design.
//
// Before including this header, the embedding translation unit must set up
// the wasm2c preamble the same way it would before including
// rlbox_wasm2c_sandbox.hpp directly:
//   #define RLBOX_WASM2C_MODULE_NAME <module>
//   #include "<module>.wasm.h"
// Do NOT define RLBOX_USE_STATIC_CALLS() — the meta resolves symbols
// dynamically per backend at dispatch time (see the
// RLBOX_USE_STATIC_CALLS gotcha in CLAUDE.md).

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
// Must match RLBOX_WASM2C_MODULE_FUNC in rlbox_wasm2c_sandbox.hpp:
// module name is mangled as $<name>.wasm → "0x24<name>0x2Ewasm", and
// exports are emitted as "w2c_<mangled>_<funcname>".  If that macro
// formulation changes upstream, update here.
#define RLBOX_META_WASM_PREFIX                                                 \
  "w2c_0x24" RLBOX_META_STRINGIFY(RLBOX_WASM2C_MODULE_NAME) "0x2Ewasm_"

namespace rlbox {

// Which underlying backend handles a given invoke.
enum class meta_backend
{
  process,
  wasm,
};

// Per-call context the policy hook sees.  M3 shipped with just the
// resolved symbol name; M7 adds a snapshot of the rolling latency
// history (sample count and median, per backend) plus a flag saying
// whether the symbol resolves in the wasm module at all.  Snapshot
// rather than by-reference so the policy doesn't have to worry about
// the stats being mutated under it on the hot path.
//
// For M5, the allocation path also calls the policy with func_name ==
// nullptr to indicate "this decision is about where to allocate, not
// where to dispatch a call".  Latency fields are zero in that case —
// allocation has no per-symbol history.  Policies that care only about
// invokes can ignore the nullptr case and fall back to a default
// backend.
struct meta_policy_context
{
  const char* func_name;  // resolved symbol name (nullptr for allocation path)
  bool has_wasm = false;  // true iff the symbol resolves on the wasm side
  size_t proc_samples = 0;
  size_t wasm_samples = 0;
  double proc_median_ms = 0.0;
  double wasm_median_ms = 0.0;

  // Populated only on the alloc path (func_name == nullptr).  alloc_size
  // is the bytes requested; proc_wins/wasm_wins count symbols (that
  // resolve on both backends and have at least one sample on each)
  // whose current median is lower on the respective side.  A weak signal
  // — alloc-time routing is a guess because the consumer symbol isn't
  // known yet — but it's the only global head-to-head signal available
  // without per-call hints.  Zero on the invoke path.
  size_t alloc_size = 0;
  size_t proc_wins = 0;
  size_t wasm_wins = 0;
};

using meta_policy_fn = std::function<meta_backend(const meta_policy_context&)>;

class rlbox_meta_sandbox;  // fwd-decl for the struct translator types below

// M9: struct-ABI translation.  Returned by a struct_translator on the
// wasm dispatch path for each T_PointerType arg that matches a
// registered struct allocation.  wasm_scratch is the tagged meta
// T_PointerType to pass to wasm in place of the host-layout arg (so
// META_TAG_WASM must be set); cleanup is an RAII closure that runs
// after the wasm invoke returns to sync wasm-layout back to host-
// layout and free the scratch.  cleanup may be empty if no post-
// invoke sync is needed.
struct meta_struct_translation
{
  uintptr_t wasm_scratch;
  std::function<void()> cleanup;
};

// Translator hook.  The meta invokes this per-arg inside
// impl_invoke_with_func_ptr when routing to wasm.  host_ptr is the
// tagged meta T_PointerType the caller would have passed to wasm; the
// translator allocates a wasm-ABI scratch, copies fields, and returns
// the replacement pointer.  One translator covers every registered
// struct type the embedder knows about — switch on alloc size or the
// symbol name as needed.
using meta_struct_translator_fn = std::function<meta_struct_translation(
  rlbox_meta_sandbox& meta,
  const char* symbol,
  uintptr_t host_ptr)>;

// Adaptive policy: explore each backend at least `explore_per_backend`
// times per symbol, then route each subsequent call to whichever
// backend has the lower median.  The allocation path (M8) uses the
// aggregate proc_wins/wasm_wins signal: once wasm is winning more
// symbols than process across the cache, new allocations follow, and
// subsequent pointer-carrying invokes get pinned there via the
// ownership-override.
//
// Two bootstrap knobs beyond pure M8 behavior:
//
//   `alloc_warmup` (M10 follow-up, 1b in the roadmap): the first N
//   allocations to hit the alloc path with no head-to-head signal go
//   to wasm instead of the safe-default process.  Rationale: many
//   workloads allocate before any invoke has run (zlib allocates a
//   z_stream before any deflate call), so without this probe the
//   adaptive policy pins the entire session to process via ownership-
//   wins — you never get to observe wasm's latency and wasm_wins
//   stays 0 forever.  Wasm-probe bootstrap means those first allocs
//   pin consumer invokes to wasm, populating wasm's rings; subsequent
//   allocs use the normal win-count routing.  If wasm is actually
//   slower, the ownership pin on those consumer invokes makes the
//   workload eat the probe cost — priced in for the chance to flip.
//   Set to 0 to revert to pure M8 behavior (tie → process).
//
//   `reexplore_period` (M7 follow-up): once both backends have been
//   explored on a symbol, every Nth invoke routes to whichever side
//   currently has fewer samples in its ring, refreshing the data.
//   Covers drift scenarios (e.g. fork-cost amortization kicking in
//   only after the first several hundred invokes) where a committed
//   pick could otherwise stay committed indefinitely despite the
//   other backend getting faster.  0 disables; default also 0 because
//   deterministic bench repeatability matters more than drift
//   detection for the current workloads.  When non-zero, every Nth
//   post-exploration invoke probes the backend with fewer ring
//   samples (median itself is already a lifetime-lagged signal, but
//   this keeps the *sample count* symmetric so one backend's median
//   doesn't go arbitrarily stale).
inline meta_policy_fn make_adaptive_policy(size_t explore_per_backend = 3,
                                           size_t alloc_warmup = 1,
                                           size_t reexplore_period = 0)
{
  // Per-policy state — lambda captures a shared_ptr so copies of the
  // returned policy (if the caller hangs onto one and also calls
  // set_policy) share the same counters.
  struct adaptive_state
  {
    size_t alloc_no_signal = 0;  // allocs seen with wins all-zero
    size_t invoke_post_explore = 0;  // invokes seen after both sides explored
  };
  auto state = std::make_shared<adaptive_state>();
  return [explore_per_backend, alloc_warmup, reexplore_period, state](
           const meta_policy_context& ctx) -> meta_backend {
    if (ctx.func_name == nullptr) {
      // Alloc path: bias toward whichever backend is winning more
      // head-to-head symbol races.  Strict inequality required for
      // each side — ties fall through to the warmup / safe-default
      // branch.
      if (ctx.wasm_wins > ctx.proc_wins) {
        return meta_backend::wasm;
      }
      if (ctx.proc_wins > ctx.wasm_wins) {
        return meta_backend::process;
      }
      // No head-to-head signal yet.  Probe wasm for the first
      // `alloc_warmup` allocs to seed its latency rings; after that,
      // fall back to process.
      size_t n = state->alloc_no_signal++;
      if (n < alloc_warmup) {
        return meta_backend::wasm;
      }
      return meta_backend::process;
    }
    if (!ctx.has_wasm) {
      return meta_backend::process;
    }
    if (ctx.proc_samples < explore_per_backend) {
      return meta_backend::process;
    }
    if (ctx.wasm_samples < explore_per_backend) {
      return meta_backend::wasm;
    }
    // Both sides explored; optionally probe the current loser every
    // `reexplore_period` invokes so a winning backend's lead stays
    // honest — if the other backend gets faster later (fork
    // amortization, cache warmup, whatever) its ring still refreshes
    // rather than going arbitrarily stale.
    if (reexplore_period != 0) {
      size_t n = state->invoke_post_explore++;
      if ((n % reexplore_period) == 0) {
        return ctx.proc_median_ms <= ctx.wasm_median_ms ? meta_backend::wasm
                                                        : meta_backend::process;
      }
    }
    return ctx.proc_median_ms <= ctx.wasm_median_ms ? meta_backend::process
                                                    : meta_backend::wasm;
  };
}

class rlbox_meta_sandbox
{
public:
  // Integer widths follow the process backend (host-native 64-bit).  Wasm
  // dispatch narrows at the boundary in wasm_sbx.impl_invoke_with_func_ptr.
  using T_LongLongType = rlbox_process_sandbox::T_LongLongType;
  using T_LongType = rlbox_process_sandbox::T_LongType;
  using T_IntType = rlbox_process_sandbox::T_IntType;
  using T_PointerType = rlbox_process_sandbox::T_PointerType;
  using T_ShortType = rlbox_process_sandbox::T_ShortType;

  // Bit 63 encodes the owning backend of a T_PointerType: 0 for process
  // (natural — x86_64 canonical user VAs never set bit 63), 1 for wasm
  // (wasm2c offsets are 32-bit, so bit 63 is unused).  Kept in the value
  // itself rather than a side table so pointer values that survive
  // through arithmetic, struct fields, or libffi ABI remain
  // self-identifying.  A consequence: process-side allocations returned
  // from process_sbx.impl_malloc_in_sandbox must NEVER set bit 63 (true
  // in practice on x86_64 user space — if that ever changes we'd need to
  // squeeze the tag into a different bit or use a side table).
  static constexpr T_PointerType META_TAG_WASM =
    static_cast<T_PointerType>(1) << 63;
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
  // Exposed for struct-translator hooks (M9): the translator lives in
  // the embedder TU but needs to allocate wasm scratch and resolve
  // host VAs for host-layout buffers that may live on either side.
  rlbox_process_sandbox& get_process_sbx() { return process_sbx; }
  rlbox_wasm2c_sandbox& get_wasm_sbx() { return wasm_sbx; }

protected:

  static double monotonic_ms()
  {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
  }

  // Fixed-size ring of recent latencies (milliseconds).  Size chosen
  // small on purpose — we want the median to track *current* steady
  // state, not lifetime averages, so that a transition (e.g. fork()
  // amortization kicking in) can move the needle within a few calls.
  // Median over an even count picks the lower-middle element.
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
  // impl_lookup_symbol; RLBox caches it, so lifetime is tied to this
  // sandbox instance.  Holds both backends' resolved pointers so the
  // policy can pick at invoke time without re-resolving.  M7: also
  // holds per-backend rolling latency rings, populated on every invoke
  // through this symbol, read by the policy via meta_policy_context.
  struct meta_symbol_record
  {
    std::string name;
    void* process_sym = nullptr;
    void* wasm_sym = nullptr;
    mutable std::mutex stats_mutex;
    latency_ring process_latency;
    latency_ring wasm_latency;

    // Symbol pinning (perf fast-path #1).  Once the slow path has
    // dispatched to the same backend `pin_threshold_` times in a row,
    // `pinned` flips from 0 to 1=process / 2=wasm.  The hot path reads
    // `pinned` lock-free with acquire ordering; when non-zero it skips
    // the policy context build, policy call, ownership-override fold,
    // and the sample-push RAII — for zlib this collapses each invoke's
    // meta-layer overhead to a pointer deref, one atomic load, and the
    // translate() fold (which stays because it's correctness-critical
    // on the wasm path: struct-ABI translation + callback rewrite).
    //
    // `consecutive_same_picks` and `last_pick` are guarded by
    // stats_mutex (same lock the sample-push already takes), so the
    // slow-path cost of tracking adds ~nothing on top of what it was
    // already doing.  Once `pinned` is set, neither field is read
    // again for that symbol's lifetime (unless explicit unpin lands).
    std::atomic<uint8_t> pinned{0};
    uint32_t consecutive_same_picks = 0;
    meta_backend last_pick = meta_backend::process;
  };

  std::mutex symbol_cache_mutex;
  std::unordered_map<std::string, std::unique_ptr<meta_symbol_record>>
    symbol_cache;

  // Per-allocation ownership registry.  Populated on impl_malloc_in_sandbox,
  // cleared on impl_free_in_sandbox.  impl_invoke_with_func_ptr consults
  // this to override the policy when pointer args are owned by a backend
  // other than the policy's pick.  Keys are the full tagged T_PointerType
  // values — process VAs with bit 63 clear, wasm offsets widened and
  // OR'd with META_TAG_WASM — so the two address spaces can't collide.
  std::mutex alloc_mutex;
  std::unordered_map<T_PointerType, meta_backend> alloc_owner;

  // M9/M10: struct-ABI translation.  struct_types holds one entry per
  // registered (symbol, host_size, wasm_size, translator) tuple; the
  // vector is indexed by host_size through struct_types_by_size so
  // `impl_malloc_in_sandbox` can locate candidates from the byte-count
  // rlbox hands it.  When a single host_size maps to multiple symbols
  // we have no way to disambiguate at alloc-time (rlbox's malloc API
  // surfaces only the size), so the dispatch path aborts with a
  // diagnostic pointing to the colliding symbols — safer than silently
  // running the wrong translator against a user buffer.  For libraries
  // whose struct sizes are all distinct, lookup collapses to a single
  // candidate and this mirrors the pre-M10 behavior exactly.
  //
  // struct_allocs maps each live tagged host allocation to its stable
  // co-allocated wasm scratch plus a copy of the translator function
  // to call on invoke.  The scratch is allocated once at
  // impl_malloc_in_sandbox and freed at impl_free_in_sandbox; the
  // translator copies fields per invoke but the offset itself never
  // moves.  Stability is load-bearing: zlib's deflate_state caches
  // `strm` (= the scratch offset used at deflateInit) and checks it
  // against the `strm` passed to every subsequent deflate call; a
  // scratch that churns per-invoke trips that check.
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

  // Dispatch counters for tests and benchmarks.  Incremented inside
  // impl_malloc_in_sandbox / impl_invoke_with_func_ptr right before
  // forwarding to the chosen backend, so they reflect the post-override
  // (ownership-wins) choice rather than the raw policy pick.
  size_t mallocs_on_process_ = 0;
  size_t mallocs_on_wasm_ = 0;
  size_t invokes_on_process_ = 0;
  size_t invokes_on_wasm_ = 0;

  // Eager dual callback registration (M6).  impl_register_callback
  // installs the host function on *both* underlying backends so either
  // can invoke it, and records the pairing here.  The map key is the
  // process backend's trampoline address (stable, non-zero, distinct
  // from any wasm slot index because it's a real host VA), which is
  // also the value returned from impl_register_callback as the meta-
  // level handle.  Wasm-dispatch consults this map to rewrite a handle
  // arg into its wasm slot index before narrowing to the wasm backend.
  struct meta_callback_record
  {
    rlbox_wasm2c_sandbox::T_PointerType wasm_slot;
    void* key;
  };
  std::mutex callback_mutex;
  std::unordered_map<T_PointerType, meta_callback_record> callback_registry;

  meta_policy_fn policy;

  // After this many consecutive slow-path dispatches to the same backend
  // for a given symbol, the symbol's `pinned` flag is set and subsequent
  // invokes take the lock-free fast path — no policy call, no ownership
  // fold, no sample push.  0 disables pinning entirely (every invoke
  // walks the full slow path).  Keep non-zero in tandem with adaptive's
  // `reexplore_period`: once pinned, the policy is no longer consulted,
  // so re-exploration won't fire on that symbol.  Default tuned for
  // zlib (adaptive converges within ~6 invokes per symbol; 16 gives
  // enough slack that bursty ownership-override flips don't toggle the
  // pin off-and-on without ever sticking).
  uint32_t pin_threshold_ = 16;

public:
  rlbox_meta_sandbox()
    // Default: route every call to the process backend.  Matches M2
    // behavior and keeps pointer-heavy workloads (zlib) working until
    // M4/M5 land.
    : policy([](const meta_policy_context&) { return meta_backend::process; })
  {}

  // Embedder hook: override the per-call dispatch policy.  Called on the
  // hot path of every invoke, so keep it cheap.
  void set_policy(meta_policy_fn new_policy)
  {
    policy = std::move(new_policy);
  }

  // Static registry of live meta instances, keyed by the underlying
  // `rlbox_process_sandbox*`.  Used by
  // `impl_get_executed_callback_sandbox_and_key` to translate a
  // process-backend TLS hit (the process backend publishes its own
  // `rlbox_process_sandbox*` there) back into the owning
  // `rlbox_meta_sandbox*` the rlbox interceptor actually wants.  The
  // registry is necessary on the process side because process
  // callbacks dispatch on a dedicated host-side callback thread
  // (see `start_callback_loop`), which means TLS set on the invoker
  // thread doesn't reach them.  The wasm side doesn't need a
  // registry because wasm2c dispatches callbacks synchronously on
  // the invoker's thread, so the meta can set its own TLS at
  // wasm-invoke entry and read it at interceptor time.
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

  // TLS for the wasm-invoke callback path.  Set on entry to the wasm
  // branch of `impl_invoke_with_func_ptr`, cleared on exit via RAII.
  // If a wasm-side callback fires mid-invoke, the interceptor reads
  // this to identify the meta.  `inline` so multiple TUs don't
  // collide on linkage.
  static inline thread_local rlbox_meta_sandbox* tl_invoking_meta = nullptr;

  // Stand up both backends in parallel.  The library path is the native
  // shared library the process backend loads into the child; the wasm
  // module is statically linked into this binary, so wasm2c's
  // impl_create_sandbox takes no positional args.  If the wasm side fails
  // we tear the process side back down to keep the meta-sandbox's
  // "either both up or neither up" invariant.
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
      // Wasm-owned: strip the tag and narrow to wasm's 32-bit offset
      // before handing to the wasm backend's resolver, which maps
      // offset → host VA in wasm linear memory.
      return wasm_sbx.impl_get_unsandboxed_pointer<T>(
        static_cast<rlbox_wasm2c_sandbox::T_PointerType>(tag_strip(p)));
    }
    return process_sbx.impl_get_unsandboxed_pointer<T>(p);
  }

  template<typename T>
  inline T_PointerType impl_get_sandboxed_pointer(const void* p) const
  {
    // Host VA → tagged T_PointerType.  tainted<T*> stores the host VA;
    // RLBox converts back to T_PointerType through this hook at the
    // invoke boundary.  If the VA lies inside wasm's linear memory,
    // translate via wasm_sbx and tag; otherwise fall back to process.
    // const_cast because wasm_sbx's membership check is non-const
    // upstream even though it reads values that don't change after
    // impl_create_sandbox.
    auto& ws = const_cast<rlbox_wasm2c_sandbox&>(wasm_sbx);
    if (ws.impl_is_pointer_in_sandbox_memory(p)) {
      auto raw = wasm_sbx.impl_get_sandboxed_pointer<T>(p);
      return tag_wasm(static_cast<T_PointerType>(raw));
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

  // 3-arg form so rlbox hands us `expensive_sandbox_finder`, which
  // resolves a pointer to the owning `rlbox_meta_sandbox*` (if any) by
  // walking the registered-sandbox list and calling each sandbox's
  // `is_pointer_in_sandbox_memory`.  Our instance version already
  // union-checks process + wasm, so the finder returns a non-null meta
  // iff the pointer lives in *either* backend of that meta.
  //
  // Cases, with s_i = finder(p_i):
  //   s1 == nullptr && s2 == nullptr — both app memory → same
  //     (trivially; rlbox's contract treats host memory as one sandbox-
  //      equivalent address space for range-check purposes)
  //   s1 != s2                        — mixed app/sandbox or distinct
  //                                     sandbox instances → different
  //   s1 == s2 (non-null)             — both in this meta, but may be
  //                                     in different backends — so
  //                                     tiebreak on which backend's
  //                                     memory each pointer lands in
  //
  // The backend-disambiguation step is why this can't just forward to
  // the process backend's 2-arg version: process would say "both
  // outside any process sandbox → same" for a (host, wasm) or
  // (wasm_meta_A, wasm_meta_B) pair, which isn't safe if the caller
  // later does a ranged access.
  static inline bool impl_is_in_same_sandbox(
    const void* p1,
    const void* p2,
    rlbox_meta_sandbox* (*expensive_sandbox_finder)(
      const void* example_sandbox_ptr))
  {
    // find_sandbox_from_example dynamic_checks against null — guard
    // here so we match the process backend's null-tolerant contract
    // (two null pointers compare "same sandbox" trivially).
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
    // RLBOX_USE_STATIC_CALLS on, which we can't use here — see CLAUDE.md).
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
  // wasm dispatch path — the process path passes handles through
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

    // Fast path: once a symbol has pinned to a backend, skip policy
    // context build, policy call, ownership-override fold, and the
    // sample-push RAII (no clock_gettime, no stats_mutex grab, no ring
    // write).  Correctness-critical work stays: struct-ABI translation
    // and callback-slot rewrite on the wasm path, and the tl_invoking_
    // meta RAII so a wasm-dispatched host-callback can still resolve
    // its owning meta.  Ownership-override is deliberately skipped here
    // — if workload invariants change such that cross-backend pointer
    // flows appear, either raise pin_threshold_ so the slow path runs
    // longer before pinning, or set pin_threshold_ = 0 to disable.
    uint8_t pin = rec->pinned.load(std::memory_order_acquire);
    if (pin == 2) {
      invokes_on_wasm_++;
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
      return wasm_sbx.impl_invoke_with_func_ptr<T, T_Converted>(
        reinterpret_cast<T_Converted*>(rec->wasm_sym),
        translate(std::forward<T_Args>(params))...);
    }
    if (pin == 1) {
      invokes_on_process_++;
      return process_sbx.impl_invoke_with_func_ptr<T, T_Converted>(
        reinterpret_cast<T_Converted*>(rec->process_sym),
        std::forward<T_Args>(params)...);
    }

    // Slow path: full policy consultation, ownership override, timing.
    // After enough consecutive dispatches to the same backend, the
    // sample_pusher will flip `rec->pinned` and future invokes will
    // take the fast path above.

    // Build the policy context with a snapshot of current latency
    // history.  Lock-hold is cheap (copy four numbers and a bool) and
    // keeps the snapshot internally consistent: the policy sees a
    // coherent (count, median) pair per backend rather than a count
    // from one moment and a median from another.
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
    meta_backend choice = policy(ctx);

    // Pointer-ownership override: if any pointer-typed arg belongs to a
    // registered allocation, its owning backend wins over the policy
    // pick.  M5 makes this bidirectional — the registry can now hold
    // both process VAs and tagged wasm offsets, so a wasm-owned buffer
    // under a process-forcing policy routes to wasm just as a process-
    // owned buffer under a wasm-forcing policy routes to process.
    // If two args end up claimed by different backends there's no valid
    // route; abort rather than silently corrupt.
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

    // RAII sample pusher: times the forwarded call and records the
    // result against the chosen backend when the scope exits.  Works
    // uniformly for void and non-void invokes because the destructor
    // runs after the return expression evaluates but before control
    // returns to the meta's caller.  Also updates the pin counter
    // under the same lock hold — folding it into the existing
    // critical section adds ~nothing over what the ring push was
    // already doing.
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
      // Per-arg translate: subsumes callback-slot rewrite and M9
      // struct-ABI translation.  For each T_PointerType arg:
      //   1. If it matches a registered struct allocation and a
      //      translator is installed, call the translator and push
      //      its cleanup closure; the returned wasm scratch pointer
      //      is the value forwarded to wasm.
      //   2. Else if it matches a registered callback handle, swap
      //      in the paired wasm slot index.
      //   3. Else pass through verbatim — the uintptr_t → uint32_t
      //      narrowing at the wasm invoke boundary strips any
      //      META_TAG_WASM bit for free.
      // Non-pointer args bypass translation entirely.
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
          return val;
        } else {
          return static_cast<A>(std::forward<decltype(a)>(a));
        }
      };

      // Order matters: sp is destroyed *after* cleanup_runner (reverse
      // declaration order), so the latency sample covers the whole
      // host→wasm→host round-trip including translation cost.
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
      // RAII-stash the invoking meta for the duration of the wasm
      // dispatch.  If the wasm module fires a registered host callback
      // mid-invoke (synchronous, same thread), the rlbox interceptor
      // calls `impl_get_executed_callback_sandbox_and_key`, which
      // reads this TLS to recover (meta*, user-key).
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
      return wasm_sbx.impl_invoke_with_func_ptr<T, T_Converted>(
        reinterpret_cast<T_Converted*>(rec->wasm_sym),
        translate(std::forward<T_Args>(params))...);
    }

    invokes_on_process_++;
    sample_pusher sp{ rec, meta_backend::process, monotonic_ms(),
                      pin_threshold_ };
    return process_sbx.impl_invoke_with_func_ptr<T, T_Converted>(
      reinterpret_cast<T_Converted*>(rec->process_sym),
      std::forward<T_Args>(params)...);
  }

  // Walk the symbol cache and count, across all symbols that resolve
  // on both backends AND have ≥1 latency sample on each side, how many
  // have a lower median on process vs wasm.  Used only on the alloc
  // path — it's O(N_symbols + N_records_with_wasm), off the invoke hot
  // path.  We snapshot the record pointers under symbol_cache_mutex
  // and release it before acquiring each record's stats_mutex
  // individually, so we never nest the two locks.  Records live for
  // the sandbox's lifetime (symbol_cache only grows), so the raw
  // pointers captured here stay valid until destroy_sandbox.
  void snapshot_alloc_wins(size_t& proc_wins, size_t& wasm_wins)
  {
    proc_wins = 0;
    wasm_wins = 0;
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
      if (pm <= wm) ++proc_wins;
      else ++wasm_wins;
    }
  }

  inline T_PointerType impl_malloc_in_sandbox(size_t size)
  {
    // Policy gets called with func_name == nullptr so allocation-aware
    // policies can distinguish alloc vs invoke.  Snapshot the aggregate
    // per-backend wins across the symbol cache so a policy like
    // make_adaptive_policy can bias toward whichever backend is faster
    // overall.  Cost is off-hot-path (alloc, not invoke) and bounded by
    // the number of resolved symbols.
    meta_policy_context ctx{};
    ctx.func_name = nullptr;
    ctx.alloc_size = size;
    snapshot_alloc_wins(ctx.proc_wins, ctx.wasm_wins);
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
      // Sanity: process backend must not hand back a pointer that
      // collides with the wasm tag bit.  On x86_64 user space this
      // should be impossible; assert to catch the day it stops being.
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
    // wasm scratch and record the translator with the allocation.
    // alloc_mutex is released before the wasm alloc runs so we don't
    // hold it across an arbitrary backend call; we re-acquire briefly
    // to publish the pair.
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
          "types — cannot disambiguate translator at alloc time.  "
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
    meta_backend owner = tag_owner(p);
    T_PointerType raw = tag_strip(p);
    uint32_t scratch_to_free = 0;
    {
      std::lock_guard<std::mutex> g(alloc_mutex);
      alloc_owner.erase(p);
      auto sit = struct_allocs.find(p);
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

  // Introspection for tests / policy code.  Returns the owning backend of
  // a top-level allocation, or nullopt if the pointer wasn't produced by
  // this meta's malloc_in_sandbox (e.g. interior pointer, nullptr,
  // externally-derived).
  //
  // Intentionally does NOT locate which backend a random address belongs
  // to — that's a different question (membership), answered by
  // impl_is_pointer_in_sandbox_memory.
  std::pair<bool, meta_backend> owner_of(T_PointerType p)
  {
    std::lock_guard<std::mutex> g(alloc_mutex);
    auto it = alloc_owner.find(p);
    if (it == alloc_owner.end()) {
      return { false, meta_backend::process };
    }
    return { true, it->second };
  }

  // Dispatch counters — useful for tests that want to assert routing
  // without inspecting pointer values, and for future bench harnesses.
  size_t mallocs_on_process() const { return mallocs_on_process_; }
  size_t mallocs_on_wasm() const { return mallocs_on_wasm_; }
  size_t invokes_on_process() const { return invokes_on_process_; }
  size_t invokes_on_wasm() const { return invokes_on_wasm_; }

  // Tunable: consecutive same-backend dispatches before a symbol pins.
  // Set to 0 to disable the fast path (every invoke walks the slow
  // path and can re-consult the policy).  See `pin_threshold_` comment
  // for the interaction with adaptive's `reexplore_period`.
  void set_pin_threshold(uint32_t n) { pin_threshold_ = n; }
  uint32_t pin_threshold() const { return pin_threshold_; }

  // Returns the current pin state for `func_name`: process, wasm, or
  // nullopt if the symbol hasn't been pinned (or hasn't been looked up
  // at all).  Exposed for tests and benchmarks — not read on the hot
  // path, which loads the atomic directly.
  std::optional<meta_backend> pinned_backend_for(const char* func_name)
  {
    std::lock_guard<std::mutex> g(symbol_cache_mutex);
    auto it = symbol_cache.find(func_name);
    if (it == symbol_cache.end()) return std::nullopt;
    uint8_t p = it->second->pinned.load(std::memory_order_acquire);
    if (p == 0) return std::nullopt;
    return p == 1 ? meta_backend::process : meta_backend::wasm;
  }

  // M9/M10: struct-ABI translation API.  Register each struct type
  // the embedder wants to cross the meta→wasm boundary with its host
  // size (what impl_malloc_in_sandbox will see — usually sizeof(T)
  // at the call site), wasm scratch size
  // (sizeof(Sbx_<lib>_<struct><rlbox_wasm2c_sandbox>)), and the
  // translator.  Each matching allocation silently co-allocates a
  // stable wasm scratch of wasm_size and pins that translator to
  // the allocation; on every wasm dispatch carrying the allocation,
  // the translator runs to populate the scratch and returns a
  // cleanup closure that mirrors state back.  Scratch is never
  // alloc/freed per invoke — stability matters (see struct_allocs
  // comment).  No-op on the process dispatch path.
  //
  // `symbol` is the library-level struct name (e.g. "z_stream_s"); the
  // `rlbox_meta_load_struct_translators` macro passes `#T_Struct` from
  // the X-macro, so embedders hand-rolling this API should mirror the
  // struct-reflection tag.  It's used (a) as a human-readable label in
  // the collision diagnostic if two registrations share host_size and
  // (b) to let a repeated registration of the same symbol silently
  // replace its predecessor (the macro can be called twice during
  // iterative development without stacking entries).  A size collision
  // across *distinct* symbols is still a hard error at alloc time —
  // rlbox's malloc API surfaces only the size, so we can't pick the
  // right translator without more information.  When that collision
  // shows up in a real library, the fix is to extend the alloc API
  // with a type hint (e.g. malloc_typed<T>), but for now zlib's single
  // struct keeps this off the critical path.
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
  // Returns the stable wasm-scratch offset for a registered struct
  // allocation, or 0 if p isn't a struct allocation.  The translator
  // calls this each invoke to find the pre-allocated scratch.
  uint32_t get_struct_scratch(T_PointerType p)
  {
    std::lock_guard<std::mutex> g(alloc_mutex);
    auto it = struct_allocs.find(p);
    if (it == struct_allocs.end()) return 0;
    return it->second.wasm_scratch;
  }

  // Per-symbol latency snapshot.  Looks up the symbol in the cache
  // (must have been resolved at least once via a prior invoke) and
  // returns the same numbers the policy hook would see.  Intended for
  // tests and introspection; policies should read via
  // meta_policy_context instead of calling this, to avoid a redundant
  // symbol_cache_mutex acquisition on the hot path.
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
    // Register on both backends up front so either dispatch path can
    // fire the callback without an extra setup round-trip.  Order
    // matters only for rollback shape — if process succeeds and wasm
    // aborts (wasm2c uses dynamic_check→abort on slot exhaustion), the
    // whole process is dead and the process-side trampoline leaks
    // inside a sandbox that's about to die.  Not worth a partial-
    // rollback dance.
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
    // Drop our side-registry entry first (walking by key because the
    // registry is keyed by proc_ptr; callbacks are few, linear scan is
    // fine).  Then tear down on both backends so neither leaks a slot.
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

  // Number of live registered callbacks.  Test / introspection hook —
  // M6 verifies dual registration by checking this grows on register
  // and shrinks on unregister without depending on backend internals.
  size_t callbacks_registered() const { return callback_registry.size(); }

  // sandbox_callback_interceptor in rlbox_sandbox.hpp calls this when a
  // registered host callback fires, so it can recover (sandbox, key)
  // — which for the meta must be (rlbox_meta_sandbox*, user_fn_ptr).
  //
  // Two paths, distinguished by the backend that fired:
  //
  //   Process — dispatch runs on the process backend's callback
  //     thread, which publishes (sandbox, key) via
  //     `detail::thread_local_sandbox` + `thread_local_callback_key`
  //     right before invoking the interceptor.  The meta's invoker
  //     thread TLS isn't visible here, so we map
  //     `rlbox_process_sandbox*` → meta through `process_to_meta_map`
  //     (populated at impl_create_sandbox).
  //
  //   Wasm — dispatch is synchronous on the invoker thread, so the
  //     meta's own `tl_invoking_meta` TLS set at wasm-invoke entry
  //     is still live.  wasm2c's own `impl_get_*` is safe to call
  //     *only* when we know we're inside a wasm invoke on this
  //     thread — otherwise it dereferences its null-initialized
  //     TLS sandbox pointer.  The TLS guard gives us that check.
  //
  // Priority order matters: check process first.  A process callback
  // runs on a different thread than any in-flight wasm invoke on the
  // current thread, so there's no conflict — but if both sides were
  // checked in the wrong order and a wasm invoke happened to be in
  // flight on this thread, we'd ignore the actual process-side firer.
  // Process's reader returns (nullptr, nullptr) when no callback is
  // active on this thread, which makes the fall-through clean.
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

// M10: struct-translator macros.  These walk the same
// `sandbox_fields_reflection_<libId>_allClasses` /
// `sandbox_fields_reflection_<libId>_class_<struct>` X-macros that
// `rlbox_load_structs_from_library` consumes, and emit one translator
// function per struct plus a single `meta_<libId>_setup(meta&)`
// function that registers all of them via register_struct_type.
//
// Expected usage, in the embedder TU, AFTER
// `rlbox_load_structs_from_library(<libId>)`:
//
//   rlbox_meta_load_struct_translators(<libId>);
//
// and then in main() / sandbox-setup code:
//
//   rlbox::meta_<libId>_setup(*sandbox.get_sandbox_impl());
//
// The per-struct translator body mirrors the hand-written MVP: it
// resolves host/wasm views via the meta's impl_get_unsandboxed_pointer
// on the stable scratch from get_struct_scratch, copies host→wasm
// via the struct's field-reflection X-macro, and returns a cleanup
// closure that copies wasm→host on invoke return (no scratch
// alloc/free per call — the meta co-allocates at malloc and holds
// until free).

// Field-copy expansion helper — used by both the forward and reverse
// field walks.  See the hand-written translator comment in
// main_meta.cpp for why decltype(dst->field) + static_cast is well-
// defined for every field rlbox emits into Sbx_<struct><T_Sbx>.
#define rlbox_meta_copy_field_helper(TYPE, NAME, ATTR, ...)                    \
  dst->NAME = static_cast<decltype(dst->NAME)>(src->NAME);
#define rlbox_meta_nosep_helper()

// Emit one translator function.  Invoked indirectly by the
// all-classes macro with (T_StructName, libId).  The generated
// symbol is `meta_struct_translator_<libId>_<T_StructName>` and
// lives inside `namespace rlbox` so its Sbx_<…> references resolve
// without qualification.
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
                 ": no scratch registered for allocation — did you forget "    \
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

// Register one struct's translator with the meta.  Invoked indirectly
// by the all-classes macro from inside meta_<libId>_setup.  The symbol
// tag (first arg) is derived from the struct name that rlbox's own
// sandbox_fields_reflection macro already hands us, so it stays in
// sync with the struct-translator function name.
#define rlbox_meta_register_one_struct(T_Struct, libId)                        \
  meta.register_struct_type(                                                   \
    #libId "::" #T_Struct,                                                     \
    sizeof(Sbx_##libId##_##T_Struct<rlbox_process_sandbox>),                   \
    sizeof(Sbx_##libId##_##T_Struct<rlbox_wasm2c_sandbox>),                    \
    meta_struct_translator_##libId##_##T_Struct);

// Top-level: emit all translators + the setup function.  User calls
// this once per library at namespace scope, after
// rlbox_load_structs_from_library.  Requires access to the library's
// `sandbox_fields_reflection_<libId>_allClasses` macro (typically
// brought in by the same header that supplies the per-struct field
// reflection).
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

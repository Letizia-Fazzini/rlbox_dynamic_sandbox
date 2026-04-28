// Unit tests for the adaptive policy factories in rlbox_meta_sandbox.hpp.
//
// These exercise meta_policy_fn closures directly with synthetic
// meta_policy_context values; no sandbox is created, no fork/IPC/wasm
// runtime is involved. Runs in milliseconds.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define RLBOX_SINGLE_THREADED_INVOCATIONS
#define RLBOX_WASM2C_MODULE_NAME zlib
#include "zlib.wasm.h"
#include "rlbox.hpp"
#include "rlbox_meta_sandbox.hpp"

using namespace rlbox;

static int failures = 0;

#define check(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);       \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

// Build a context with all fields zeroed except those passed in.  Keeps
// the test bodies focused on the inputs that drive each branch.
static meta_policy_context make_ctx(const char* func_name,
                                    bool has_wasm,
                                    size_t proc_samples,
                                    size_t wasm_samples,
                                    double proc_median_ms,
                                    double wasm_median_ms,
                                    size_t invoke_count = 0,
                                    double proc_median_sum = 0.0,
                                    double wasm_median_sum = 0.0)
{
  meta_policy_context ctx{};
  ctx.func_name = func_name;
  ctx.has_wasm = has_wasm;
  ctx.proc_samples = proc_samples;
  ctx.wasm_samples = wasm_samples;
  ctx.proc_median_ms = proc_median_ms;
  ctx.wasm_median_ms = wasm_median_ms;
  ctx.invoke_count = invoke_count;
  ctx.proc_median_sum = proc_median_sum;
  ctx.wasm_median_sum = wasm_median_sum;
  return ctx;
}

// --- per-function policy ----------------------------------------------------

static void test_pf_no_wasm_binding_always_process()
{
  auto policy = make_adaptive_per_function_policy(3, 0);
  auto ctx = make_ctx("sym", /*has_wasm=*/false, 0, 0, 0.0, 0.0);
  for (int i = 0; i < 50; ++i) {
    check(policy(ctx) == meta_backend::process, "no-wasm symbol must route to process");
  }
}

static void test_pf_alloc_alternates()
{
  auto policy = make_adaptive_per_function_policy(3, 0);
  auto ctx = make_ctx(nullptr, false, 0, 0, 0.0, 0.0);
  // First alloc -> process, second -> wasm, then alternating.
  meta_backend expected[6] = {
    meta_backend::process, meta_backend::wasm, meta_backend::process,
    meta_backend::wasm,    meta_backend::process, meta_backend::wasm
  };
  for (int i = 0; i < 6; ++i) {
    auto got = policy(ctx);
    check(got == expected[i], "alloc path must alternate process/wasm");
  }
}

static void test_pf_undersampled_side_routed()
{
  auto policy = make_adaptive_per_function_policy(3, 0);
  // proc has zero samples, wasm has plenty -> route to process.
  auto a = make_ctx("sym", true, 0, 8, 1.0, 1.0);
  for (int i = 0; i < 10; ++i) {
    check(policy(a) == meta_backend::process, "undersampled process side must be probed");
  }
  // Reverse: wasm under, process plenty -> wasm.
  auto b = make_ctx("sym", true, 8, 0, 1.0, 1.0);
  for (int i = 0; i < 10; ++i) {
    check(policy(b) == meta_backend::wasm, "undersampled wasm side must be probed");
  }
}

static void test_pf_both_undersampled_random_seeds_both()
{
  auto policy = make_adaptive_per_function_policy(3, 0);
  auto ctx = make_ctx("sym", true, 0, 0, 0.0, 0.0);
  bool saw_process = false, saw_wasm = false;
  for (int i = 0; i < 200 && !(saw_process && saw_wasm); ++i) {
    auto got = policy(ctx);
    if (got == meta_backend::process) saw_process = true;
    else saw_wasm = true;
  }
  check(saw_process && saw_wasm,
        "with both rings empty, random-seed must hit both backends");
}

static void test_pf_stable_phase_picks_faster_median()
{
  auto policy = make_adaptive_per_function_policy(3, 0);
  // Both fully explored, process clearly faster.
  auto a = make_ctx("sym", true, 8, 8, 1.0, 5.0);
  for (int i = 0; i < 20; ++i) {
    check(policy(a) == meta_backend::process, "lower median wins (process)");
  }
  auto b = make_ctx("sym", true, 8, 8, 5.0, 1.0);
  for (int i = 0; i < 20; ++i) {
    check(policy(b) == meta_backend::wasm, "lower median wins (wasm)");
  }
}

static void test_pf_stable_phase_tie_random()
{
  auto policy = make_adaptive_per_function_policy(3, 0);
  auto ctx = make_ctx("sym", true, 8, 8, 1.0, 1.0);
  bool saw_process = false, saw_wasm = false;
  for (int i = 0; i < 200 && !(saw_process && saw_wasm); ++i) {
    auto got = policy(ctx);
    if (got == meta_backend::process) saw_process = true;
    else saw_wasm = true;
  }
  check(saw_process && saw_wasm, "tied medians must produce both backends across trials");
}

static void test_pf_reexplore_probes_loser()
{
  auto policy = make_adaptive_per_function_policy(3, /*reexplore_period=*/100);
  // Process is the loser (higher median).  invoke_count = 100 -> Nth-call.
  auto ctx = make_ctx("sym", true, 8, 8, 5.0, 1.0, /*invoke_count=*/100);
  check(policy(ctx) == meta_backend::process,
        "reexplore must probe the slower (process) side");
  // Reverse: wasm is the loser.
  ctx = make_ctx("sym", true, 8, 8, 1.0, 5.0, /*invoke_count=*/200);
  check(policy(ctx) == meta_backend::wasm,
        "reexplore must probe the slower (wasm) side");
}

static void test_pf_reexplore_uses_invoke_count_past_ring_size()
{
  // Latency rings saturate at 8 samples each; invoke_count keeps counting.
  // Without exposing invoke_count, a reexplore_period > 16 couldn't fire.
  auto policy = make_adaptive_per_function_policy(3, /*reexplore_period=*/1000);
  // Just past the ring's lifetime view, on the period boundary.
  auto on_period = make_ctx("sym", true, 8, 8, 1.0, 5.0, /*invoke_count=*/1000);
  check(policy(on_period) == meta_backend::wasm,
        "reexplore at invoke_count=1000 must fire (loser=wasm)");
  // One off the boundary: stable phase, picks the winner (process).
  auto off_period = make_ctx("sym", true, 8, 8, 1.0, 5.0, /*invoke_count=*/1001);
  check(policy(off_period) == meta_backend::process,
        "off-boundary invokes must take the stable path");
}

static void test_pf_invoke_count_zero_skips_reexplore()
{
  // Guard against (invoke_count % period) == 0 firing on the very first
  // post-explore call.  invoke_count=0 must not trigger reexplore.
  auto policy = make_adaptive_per_function_policy(3, /*reexplore_period=*/10);
  auto ctx = make_ctx("sym", true, 8, 8, 1.0, 5.0, /*invoke_count=*/0);
  check(policy(ctx) == meta_backend::process,
        "invoke_count=0 must use stable path, not reexplore");
}

// --- global policy ---------------------------------------------------------

static void test_global_alloc_returns_current_best()
{
  auto policy = make_adaptive_global_policy(1, 0);
  // current_best initialises to process; alloc reads it without updating.
  auto alloc = make_ctx(nullptr, false, 0, 0, 0.0, 0.0);
  check(policy(alloc) == meta_backend::process,
        "fresh global policy alloc path returns initial current_best (process)");
  check(policy(alloc) == meta_backend::process,
        "alloc reads current_best without updating it");
}

static void test_global_post_warmup_picks_lower_sum()
{
  auto policy = make_adaptive_global_policy(/*explore=*/2, 0);
  // Burn through warmup with no signal; just observe the policy stops aborting.
  auto warmup = make_ctx("sym", true, 0, 0, 0.0, 0.0);
  (void)policy(warmup);
  (void)policy(warmup);
  // Past warmup with proc_median_sum < wasm_median_sum -> next pick = process.
  // Then alloc returns the new current_best.
  auto winner_proc = make_ctx("sym", true, 8, 8, 1.0, 5.0,
                              /*invoke_count=*/0,
                              /*proc_median_sum=*/3.0,
                              /*wasm_median_sum=*/9.0);
  // Drive the update; subsequent alloc reads the result.
  (void)policy(winner_proc);
  auto alloc = make_ctx(nullptr, false, 0, 0, 0.0, 0.0);
  check(policy(alloc) == meta_backend::process,
        "after wins on process, alloc routes to process");

  // Flip the signal so wasm wins, push two updates so the random tie path
  // can't dominate, and confirm alloc tracks the new winner.
  auto winner_wasm = make_ctx("sym", true, 8, 8, 5.0, 1.0,
                              0, 9.0, 3.0);
  (void)policy(winner_wasm);
  (void)policy(winner_wasm);
  check(policy(alloc) == meta_backend::wasm,
        "after wins flip to wasm, alloc routes to wasm");
}

static void test_global_no_wasm_fallback()
{
  auto policy = make_adaptive_global_policy(1, 0);
  // Force the policy into "current_best=wasm" by feeding wasm-favorable sums
  // with has_wasm=true...
  auto winner_wasm = make_ctx("sym", true, 8, 8, 5.0, 1.0, 0, 9.0, 3.0);
  for (int i = 0; i < 5; ++i) (void)policy(winner_wasm);

  // ...then call into a symbol with no wasm binding; the dispatch must
  // fall back to process for that one call.
  auto no_wasm = make_ctx("other_sym", false, 0, 0, 0.0, 0.0);
  check(policy(no_wasm) == meta_backend::process,
        "current_best=wasm but symbol has no wasm binding -> fallback to process");
}

// --- compile-flag default factory ------------------------------------------

static void test_default_factory_matches_compile_flag()
{
  auto policy = make_adaptive_policy(3, 0);
  auto alloc = make_ctx(nullptr, false, 0, 0, 0.0, 0.0);
#ifdef RLBOX_META_ADAPTIVE_PER_FUNCTION
  // Per-function alloc alternates between process and wasm.
  auto a = policy(alloc);
  auto b = policy(alloc);
  check(a != b,
        "per_function default: alloc path must alternate across two calls");
#else
  // Global alloc returns current_best, which is stable across calls when no
  // named invokes have driven an update.
  auto a = policy(alloc);
  auto b = policy(alloc);
  check(a == b, "global default: alloc path must be stable across calls");
  check(a == meta_backend::process,
        "global default: initial current_best is process");
#endif
}

int main()
{
  test_pf_no_wasm_binding_always_process();
  test_pf_alloc_alternates();
  test_pf_undersampled_side_routed();
  test_pf_both_undersampled_random_seeds_both();
  test_pf_stable_phase_picks_faster_median();
  test_pf_stable_phase_tie_random();
  test_pf_reexplore_probes_loser();
  test_pf_reexplore_uses_invoke_count_past_ring_size();
  test_pf_invoke_count_zero_skips_reexplore();

  test_global_alloc_returns_current_best();
  test_global_post_warmup_picks_lower_sum();
  test_global_no_wasm_fallback();

  test_default_factory_matches_compile_flag();

  if (failures == 0) {
    std::printf("PASS: meta policy unit tests\n");
    return 0;
  }
  std::fprintf(stderr, "FAIL: %d meta policy unit test failures\n", failures);
  return 1;
}

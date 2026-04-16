# bench/ — zlib backend benchmark harness

Compares the three zlib backends on the same workload:

- **native** — stock libz, via `bench_native` (this directory).
- **wasm2c** — RLBox wasm2c sandbox, via `test/zlib-testing/build/main`.
- **process** — this repo's process sandbox, via `test/zlib-testing/build/main_process`.

Each sandbox main emits a `SANDBOX_MS=… NATIVE_MS=…` line; `bench_native` emits `NATIVE_MS=…`. The driver parses those (plus its own wall-clock wrapper), writes a CSV, and the plot script produces graphs.

## One-shot run

```bash
# 1. Build the wasm2c variant (default zlib-testing/build).
cd test/zlib-testing
cmake -DCMAKE_BUILD_TYPE=Release -S . -B build && cmake --build build --parallel

# 2. Build a process variant per transport.  Each gets its own dir; the bench
#    driver looks for build_rpclib/ and build_capnp/ by default.
cmake -DCMAKE_BUILD_TYPE=Release -DRLBOX_TRANSPORT=rpclib -S . -B build_rpclib \
  && cmake --build build_rpclib --parallel
cmake -DCMAKE_BUILD_TYPE=Release -DRLBOX_TRANSPORT=capnp -S . -B build_capnp \
  && cmake --build build_capnp --parallel
# Seed pi.txt into each (the sandbox mains hard-code "pi.txt" in CWD).
cp build/pi.txt build_rpclib/pi.txt
cp build/pi.txt build_capnp/pi.txt

# 3. Drive the benchmark. Defaults: sizes=256K,1M,4M,16M; levels=1,6,9; 3 iters.
cd ../../bench
python3 run_benchmarks.py

# 4. Render plots.
python3 plot_results.py
```

Output lands in `bench/results.csv` and `bench/plots/*.png`.

## Flags

`run_benchmarks.py`:

- `--wasm2c-build-dir PATH` — default `../test/zlib-testing/build`.
- `--process-builds LIST` — comma-separated `label:path` or bare `subdir` (resolved under `test/zlib-testing/`). Default: `process_rpclib` + `process_capnp`.
- `--sizes 256k,1m,4m,16m` — comma-separated; `k`/`m` suffixes ok.
- `--levels 1,6,9` — comma-separated compression levels.
- `--iters N` — iterations per (backend, size, level). Median is used for plotting.
- `--no-wasm2c` / `--no-process` — skip a backend (e.g. if only one is built).

`plot_results.py`:

- `--csv PATH` — default `results.csv`.
- `--out-dir PATH` — default `plots/`.

## Inputs

The sandbox mains hard-code `pi.txt` as their input and `./sandbox_shim.so` as the preloaded shim, so the driver runs each binary from the build directory. Before each size point, it overwrites `pi.txt` with the seed text repeated to the target size. The original `pi.txt` is backed up as `pi.txt.bench_backup` and restored when the run finishes (or crashes).

## What the sandbox mains measure

Both `main.cpp` and `main_process.cpp` now accumulate time separately for the sandboxed compression work and the in-process native baseline correctness check. `SANDBOX_MS` is the time spent inside the sandbox (including `rlbox::memcpy`, `malloc_in_sandbox`, `invoke_sandbox_function`, `copy_and_verify`, and `free_in_sandbox` — i.e. everything that actually crosses the boundary). `NATIVE_MS` is the time spent in the parallel stock-libz baseline inside the same process, for sanity against `bench_native`.

## Plots

- `plots/time_vs_size.png` — log-log median compression time vs input size, one line per backend, faceted by compression level.
- `plots/overhead.png` — sandbox-to-native slowdown factor (bar chart), faceted by level.
- `plots/throughput.png` — MB/s throughput, faceted by level.

## Caveats

- The sandbox mains do a fair amount of correctness checking (copy-and-verify on every chunk). That work is *part of the sandboxing cost*, so it's included in `SANDBOX_MS` intentionally.
- `bench_native` and the sandbox mains' internal native baseline both produce a side file (`compressed_native.bin`, `compressed.txt`, `compressed_baseline.txt`) — harmless; they get overwritten each run.
- The process backend forks a child per `invoke_sandbox_function`. That cost is per-call; many small chunks amortize worse than a few large ones. The input-size sweep is what surfaces this effect.

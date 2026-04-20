# bench/ — sandbox benchmark harness

Compares three sandboxing backends on the same workload for a chosen library (zlib or libjpeg, default: **jpeg**):

- **native** — stock library, via `bench_native` in the library's build dir.
- **wasm2c** — RLBox wasm2c sandbox, via `test/<lib>-testing/build/main`.
- **process** — this repo's process sandbox, via `test/<lib>-testing/build/main_process`.

Binaries may print a `COMPRESSION_MS=…` line; the driver parses it (plus its own wall-clock wrapper), writes a CSV, and the plot script produces graphs. If no `COMPRESSION_MS=` line is printed, wall-clock time is used and noted in the summary.

## One-shot run

```bash
# 1. Build the wasm2c variant.  Replace <lib> with zlib or jpeg.
cd test/<lib>-testing
cmake -DCMAKE_BUILD_TYPE=Release -S . -B build && cmake --build build --parallel

# 2. Build a process variant per transport.  Each gets its own dir; the bench
#    driver looks for build_rpclib/ and build_capnp/ by default.
cmake -DCMAKE_BUILD_TYPE=Release -DRLBOX_TRANSPORT=rpclib -S . -B build_rpclib \
  && cmake --build build_rpclib --parallel
cmake -DCMAKE_BUILD_TYPE=Release -DRLBOX_TRANSPORT=capnp -S . -B build_capnp \
  && cmake --build build_capnp --parallel

# 3. Drive the benchmark.
#    jpeg default: sizes=256K,1M,4M; levels=90,50,25,10; 3 iters.
#    zlib default: sizes=256K,1M,4M; levels=1,6,9;       3 iters.
cd ../../bench
python3 run_benchmarks.py --library jpeg   # or --library zlib

# 4. Render plots.
python3 plot_results.py
```

Output lands in `bench/results.csv` and `bench/plots/*.png`.

## Flags

`run_benchmarks.py`:

- `--library {zlib,jpeg}` — library to benchmark (default: `jpeg`). Controls default build paths, default levels, and input-file staging logic.
- `--wasm2c-build-dir PATH` — default `test/<library>-testing/build`.
- `--process-builds LIST` — comma-separated `label:path` or bare `subdir` (resolved under `test/<library>-testing/`). Default: `process_rpclib` + `process_capnp`.
- `--sizes 256k,1m,4m` — comma-separated; `k`/`m` suffixes ok.
- `--levels LIST` — comma-separated. For zlib: compression levels 1–9 (higher = more compression); for jpeg: quality values 1–100 (higher = **less** compression). Defaults: zlib → `1,6,9`; jpeg → `90,50,25,10`.
- `--iters N` — iterations per (backend, size, level). Median is used for plotting.
- `--no-wasm2c` / `--no-process` — skip a backend (e.g. if only one is built).

`plot_results.py`:

- `--csv PATH` — default `results.csv`.
- `--out-dir PATH` — default `plots/`.

## Inputs

The sandbox mains hard-code `test_data.txt` as their input and `./sandbox_shim.so` as the preloaded shim, so the driver runs each binary from its build directory. Before each size point the driver writes a resized copy of `test/<library>-testing/test_data.txt` into each build directory.

Both `test_data.txt` files have a **header line** that is written exactly once; only the body is repeated to fill the target size:

- **zlib** — header is a single label line; body (digit lines) is repeated at the byte level.
- **jpeg** — header is `W H C` (image dimensions); body lines are repeated at the line level and the header height `H` is rewritten to match the actual number of rows included, keeping the file valid for `fscanf`-based readers.

If a `test_data.txt` already exists in a build directory it is backed up as `test_data.txt.bench_backup` and restored when the run finishes (or crashes); otherwise the staged file is deleted on cleanup.

## What the sandbox mains measure

If a binary prints `COMPRESSION_MS=<value>` to stdout, that in-binary timing is recorded under `compression_ms` in the CSV. Otherwise `compression_ms` is `null` and the summary falls back to wall-clock time (`wall_ms`). The sandbox mains' internal timing should cover everything that crosses the sandbox boundary (`rlbox::memcpy`, `malloc_in_sandbox`, `invoke_sandbox_function`, `copy_and_verify`, `free_in_sandbox`).

## Plots

- `plots/time_vs_size.png` — log-log median compression time vs input size, one line per backend, faceted by level.
- `plots/overhead.png` — sandbox-to-native slowdown factor (bar chart), faceted by level.
- `plots/throughput.png` — MB/s throughput, faceted by level.

## Caveats

- The sandbox mains do a fair amount of correctness checking (copy-and-verify). That work is part of the sandboxing cost and is included in reported timing intentionally.
- `bench_native` and the sandbox mains may produce side files (`compressed.jpeg`, `compressed.txt`, etc.) — harmless; they are overwritten each run.
- The process backend forks a child per `invoke_sandbox_function`. That cost is per-call; many small chunks amortize worse than a few large ones. The input-size sweep surfaces this effect.

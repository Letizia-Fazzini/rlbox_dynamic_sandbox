# bench/ — sandbox backend benchmark harness

Drives every supported test library (currently zlib, libjpeg) across the
three sandbox backends and emits CSVs under `bench/results/`.

## Layout

- `run_benchmarks.py` — unified dispatcher (`zlib`, `jpeg`, or both)
- `run_zlib_benchmarks.py` — zlib suite (sizes × levels × backends, incl. meta)
- `run_jpeg_benchmarks.py` — libjpeg suite (qualities × backends)
- `plot_results.py` — render plots from a CSV (auto-detects zlib vs jpeg)
- `results/` — generated CSVs (auto-created)
- `plots/` — generated PNGs (auto-created)

## Quick start

```bash
# Build each test dir first; see test/<lib>-testing/ READMEs.
cd bench
python3 run_benchmarks.py                  # runs everything
python3 run_benchmarks.py zlib             # just zlib
python3 run_benchmarks.py --iters 5 jpeg   # just jpeg, 5 iters
```

Per-suite flags (sizes, levels, qualities, `--meta-policies`, etc.) live on
the individual runners — invoke them directly:

```bash
python3 run_zlib_benchmarks.py --sizes 1m,16m --levels 6 --meta-policies process,wasm,adaptive
python3 run_jpeg_benchmarks.py --qualities 25,75
```

Unified Dispatcher Flags:
- `--library {zlib,jpeg}` — library to benchmark (default: `jpeg`). Controls default build paths, default levels, and input-file staging logic.
- `--wasm2c-build-dir PATH` — default `test/<library>-testing/build`.
- `--process-builds LIST` — comma-separated `label:path` or bare `subdir` (resolved under `test/<library>-testing/`). Default: `process_rpclib` + `process_capnp`.
- `--sizes 256k,1m,4m` — comma-separated; `k`/`m` suffixes ok.
- `--levels LIST` — comma-separated. For zlib: compression levels 1–9 (higher = more compression); for jpeg: quality values 1–100 (higher = **less** compression). Defaults: zlib → `1,6,9`; jpeg → `90,50,25,10`.
- `--iters N` — iterations per (backend, size, level). Median is used for plotting.
- `--no-wasm2c` / `--no-process` — skip a backend (e.g. if only one is built).

## What the sandbox mains measure

`main.cpp` and `main_process.cpp` (both libs) emit `COMPRESSION_MS=…` for
the sandboxed compression loop — everything that crosses the sandbox
boundary, including `rlbox::memcpy`, `malloc_in_sandbox`, `invoke_sandbox_function`,
`copy_and_verify`, and `free_in_sandbox`. `main_meta` emits `SANDBOX_MS=…`;
the driver accepts both.

For zlib, `bench_native` gives a stock-libz reference for the same input.

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
- The mains do copy-and-verify on every chunk — that cost is part of
  sandboxing and stays in `COMPRESSION_MS` intentionally.


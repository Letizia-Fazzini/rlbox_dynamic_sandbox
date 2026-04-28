# Benchmarks

This directory contains the benchmark drivers and plotting scripts used to
measure the overhead of the different RLBox sandboxing backends on real
workloads (libjpeg JPEG encoding).

There are two independent benchmark suites:

| Driver | Plotter | What it measures |
| --- | --- | --- |
| [run_process_bench.py](run_process_bench.py) | [plot_process_bench.py](plot_process_bench.py) | Compares `native`, `wasm2c`, `process (rpclib)` and `process (capnp)` against each other. Each transport lives in its own build directory. |
| [run_adaptive_bench.py](run_adaptive_bench.py) | [plot_adaptive_bench.py](plot_adaptive_bench.py) | Compares `native`, `wasm2c`, `process` and the `adaptive` meta-sandbox in a single shared build directory. |

Both drivers write a CSV to [results.csv](results.csv) (override with `--out`)
and the plotters render PNGs into [plots/](plots/).

The `--levels` flag is the JPEG quality value (1-100, higher = LESS
compression). The default levels are `90,50,25,10`. Sizes are passed via
`--sizes` and accept `k`/`m` suffixes (e.g. `--sizes 5k,250k,1m`).

---

## `run_process_bench.py` — comparing transports

This driver expects **three separate build directories** under the libjpeg
testing folder, one per transport, because the rpclib and capnp
configurations are mutually exclusive at CMake-configure time.

Run from inside [test/libjpeg-testing/](../test/libjpeg-testing/):

```bash
# wasm2c backend (also builds bench_native, used as the reference)
cmake -DCMAKE_BUILD_TYPE=Release -S . -B build_wasm
cmake --build build_wasm --parallel

# process backend with the rpclib transport
cmake -DCMAKE_BUILD_TYPE=Release -DRLBOX_TRANSPORT=rpclib -S . -B build_rpclib
cmake --build build_rpclib --parallel

# process backend with the capnp transport
cmake -DCMAKE_BUILD_TYPE=Release -DRLBOX_TRANSPORT=capnp  -S . -B build_capnp
cmake --build build_capnp  --parallel
```

After building, run the driver from this directory:

```bash
./run_process_bench.py --sizes 5k,50k,250k --iters 5
./plot_process_bench.py
```

The expected layout per build dir is:

- `build_wasm/`   contains `main` (wasm2c) and `bench_native`
- `build_rpclib/` contains `main_process` and `sandbox_shim.so` (rpclib)
- `build_capnp/`  contains `main_process` and `sandbox_shim.so` (capnp)

You can skip backends that aren't built with `--no-wasm2c`, `--no-rpclib` or
`--no-capnp`, and override individual paths with `--wasm-build-dir`,
`--rpclib-build-dir`, `--capnp-build-dir`.

For each `(backend, level, size)` cell the driver invokes the binary
`--iters` times with `num_datasets=1` and `inner_iters=1`, so each invocation
produces exactly one `COMPRESSION_MS=` measurement. The script also
temporarily replaces the `test_data/` directory in each build dir with a
single resized input file and restores the original contents on exit.

The plotter writes one `overhead_q{level}.png` per quality level, showing
slowdown of each sandbox backend versus `native`.

---

## `run_adaptive_bench.py` — comparing the adaptive meta-sandbox

This driver expects **a single build directory** (`build/`) that contains all
four binaries (`bench_native`, `main`, `main_process`, `main_adaptive`) plus
`sandbox_shim.so`. The transport for the process and adaptive backends is
whatever was selected at CMake-configure time.

From inside [test/libjpeg-testing/](../test/libjpeg-testing/):

```bash
# default transport (capnp)
cmake -DCMAKE_BUILD_TYPE=Release -S . -B build
cmake --build build --parallel

# or, to use the rpclib transport instead:
cmake -DCMAKE_BUILD_TYPE=Release -DRLBOX_TRANSPORT=rpclib -S . -B build
cmake --build build --parallel
```

Then run from this directory:

```bash
./run_adaptive_bench.py --sizes 5k,50k,250k --iters 5 --inner-iters 10
./plot_adaptive_bench.py
```

Unlike `run_process_bench.py`, this driver writes one `test_data{N}.txt` per
size and invokes each binary **once per `(backend, level)`** with
`num_datasets=len(sizes)` and `inner_iters=--inner-iters`, so a single binary
call produces `len(sizes) * inner_iters` `COMPRESSION_MS=` lines. This is the
mode the `adaptive` backend was designed for, since it lets the meta-sandbox
observe a stream of calls and switch transports in flight.

Backends can be skipped with `--no-wasm2c`, `--no-process` or `--no-adaptive`,
and the build directory can be overridden with `--wasm2c-build-dir`.

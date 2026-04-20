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

## What the sandbox mains measure

`main.cpp` and `main_process.cpp` (both libs) emit `COMPRESSION_MS=…` for
the sandboxed compression loop — everything that crosses the sandbox
boundary, including `rlbox::memcpy`, `malloc_in_sandbox`, `invoke_sandbox_function`,
`copy_and_verify`, and `free_in_sandbox`. `main_meta` emits `SANDBOX_MS=…`;
the driver accepts both.

For zlib, `bench_native` gives a stock-libz reference for the same input.

## Inputs

The zlib sandbox mains hard-code `pi.txt` in CWD; the driver seeds it by
repeating the original contents to the requested size and restores the
original after the run (even on crash). The libjpeg mains read
`rgb_grid.txt` (committed 1280×1014 RGB pixel grid — not varied).

## Caveats

- The process backend forks a child per `invoke_sandbox_function`; many
  small chunks amortize worse than a few large ones. Input-size /
  compression-level sweeps surface this.
- The mains do copy-and-verify on every chunk — that cost is part of
  sandboxing and stays in `COMPRESSION_MS` intentionally.

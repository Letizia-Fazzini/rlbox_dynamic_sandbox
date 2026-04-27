#!/usr/bin/env python3
"""
Drive the benchmark across backends for a chosen library:
    - native:   test/<lib>-testing/build/bench_native (no sandboxing at all)
    - wasm2c:   test/<lib>-testing/build/main (RLBox wasm2c sandbox)
    - process:  test/<lib>-testing/build/main_process (rpclib or capnp depending on build flags used)
    - adaptive: test/<lib>-testing/build/main_adaptive

Select the library with --library {zlib,jpeg} (default: jpeg).

For zlib: compression levels are 1-9 (higher = more compression).
For jpeg: quality levels are 1-100 (higher = LESS compression / better quality).
  Recommended quality values: 90, 50, 25, 10.

Each binary may print lines like
    COMPRESSION_MS=12.345
(one per dataset x inner iteration) which are parsed as in-binary timings;
we also record Python wall-clock time.

All binaries accept: binary quality [num_datasets] [inner_iters]
and read from test_data/test_data{d}.txt (relative to cwd).
This script creates that directory in each build dir, writes one file per size,
then invokes each binary once per (backend, level), covering all datasets in a
single call.  The test_data/ directory is backed up and restored afterwards.
"""
from __future__ import annotations

import argparse
import csv
import enum
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

BENCH_DIR = Path(__file__).resolve().parent
REPO_ROOT = BENCH_DIR.parent


class Library(enum.Enum):
    ZLIB = "zlib"
    JPEG = "jpeg"

    @property
    def testing_dir(self) -> Path:
        name = "libjpeg" if self is Library.JPEG else self.value
        return REPO_ROOT / "test" / f"{name}-testing"

    @property
    def default_levels(self) -> str:
        if self is Library.JPEG:
            return "90,50,25,10"
        return "1,6,9"


ZLIB_TESTING_DIR = Library.ZLIB.testing_dir
JPEG_TESTING_DIR = Library.JPEG.testing_dir

COMPRESSION_RE = re.compile(r"COMPRESSION_MS=([\d.]+)")


def parse_sizes(s: str) -> list[int]:
    out = []
    for chunk in s.split(","):
        c = chunk.strip().lower()
        if not c:
            continue
        if c.endswith("k"):
            out.append(int(c[:-1]) * 1024)
        elif c.endswith("m"):
            out.append(int(c[:-1]) * 1024 * 1024)
        else:
            out.append(int(c))
    return out


def parse_ints(s: str) -> list[int]:
    return [int(x.strip()) for x in s.split(",") if x.strip()]


def ensure_bench_native(build_dir: Path) -> Path:
    binary = build_dir / "bench_native"
    if not binary.exists():
        print(f"[bench] {binary} missing; rebuild with cmake", file=sys.stderr)
        sys.exit(1)
    return binary


def _write_input(target: Path, source_bytes: bytes, size: int, library: Library = Library.ZLIB) -> None:
    """Write a resized input file to `target`.

    The first line of source_bytes (the header) is written exactly once.  Only
    the body is repeated to reach approximately `size` bytes.

    For zlib, body bytes are repeated at the byte level.
    For jpeg, body lines are repeated at the line level and the header W/H/C
    line is rewritten with the actual number of rows included, so that the C
    binaries (which fscanf the header) always read a consistent file.
    """
    if not source_bytes:
        raise RuntimeError("empty source bytes -- can't repeat to target size")

    nl = source_bytes.find(b"\n")
    if nl == -1:
        raise RuntimeError("invalid data files - no header")
    else:
        header = source_bytes[:nl]
        body = source_bytes[nl + 1:]

    if library is Library.ZLIB:
        # Zlib: byte-level repetition of the body; header written once.
        if not body:
            raise RuntimeError("invalid data files - empty body")
        header_part = (header + b"\n")

        #ignoring header overhead in size computations - should be negligible
        ratio = size / len(body)
        full_reps = int(ratio)
        extra_bytes = round((ratio - full_reps) * len(body))
        blob = header_part + body * full_reps + body[:extra_bytes]
        target.write_bytes(blob)

    elif library is Library.JPEG:
        # JPEG: line-level repetition; rewrite header with actual row count.
        W, H, C = map(int, header.split())
        body_lines = [line for line in body.split(b"\n") if line]
        if not body_lines:
            raise RuntimeError("JPEG test_data.txt has no body lines to repeat")

        #ignoring header overhead in size computations - should be negligible
        body_size = len(body)
        ratio = size / body_size
        full_reps = int(ratio)
        extra_lines = round((ratio - full_reps) * len(body_lines))
        accumulated = body_lines * full_reps + body_lines[:extra_lines]
        if not accumulated:
            raise RuntimeError("Size too small for JPEG source")

        actual_H = len(accumulated)
        new_header = f"{W} {actual_H} {C}".encode()
        blob = new_header + b"\n" + b"\n".join(accumulated) + b"\n"
        target.write_bytes(blob)


def prepare_inputs_dir(build_dir: Path, source_bytes: bytes, sizes: list[int], library: Library = Library.ZLIB) -> None:
    """Create build_dir/test_data/ and write test_data{d}.txt for each size (1-indexed)."""
    data_dir = build_dir / "test_data"
    data_dir.mkdir(parents=True, exist_ok=True)
    for d, size in enumerate(sizes, start=1):
        _write_input(data_dir / f"test_data{d}.txt", source_bytes, size, library=library)


def run_once(cmd: list[str], cwd: Path) -> tuple[float, str]:
    """Run cmd under cwd, return (wall_ms, stdout_text)."""
    print(f"[bench]   -> running: {' '.join(cmd)}", file=sys.stderr)
    t0 = time.monotonic()
    res = subprocess.run(
        cmd, cwd=str(cwd), capture_output=True, text=True
    )
    wall_ms = (time.monotonic() - t0) * 1000.0
    print(f"[bench]      wall_ms={wall_ms:.1f}", file=sys.stderr)
    if res.returncode != 0:
        raise subprocess.CalledProcessError(
            res.returncode, cmd,
            output=res.stdout, stderr=res.stderr,
        )
    return wall_ms, res.stdout


def parse_timings(stdout: str) -> list[float]:
    """Return all COMPRESSION_MS= values found in stdout, in order."""
    return [float(v) for v in COMPRESSION_RE.findall(stdout)]


def run_config_multi(
    backend: str,
    cmd: list[str],
    cwd: Path,
    sizes: list[int],
    level: int,
    inner_iters: int,
    iters: int,
    warmup: bool,
) -> list[dict]:
    """Run `cmd` `iters` times (outer loop) and return a list of records.

    The binary iterates over len(sizes) datasets, each for inner_iters inner
    iterations, printing one COMPRESSION_MS= line per (dataset, inner_iter)
    pair.  Each outer run produces len(sizes) * inner_iters timing values which
    are mapped back to their respective size and inner_iter index.
    """
    expected_timings = len(sizes) * inner_iters
    print(
        f"[bench] backend={backend} sizes={sizes} level={level} "
        f"inner_iters={inner_iters} iters={iters} warmup={warmup}",
        file=sys.stderr,
    )
    if warmup:
        print(f"[bench]  [warmup]", file=sys.stderr)
        t0 = time.monotonic()
        run_once(cmd, cwd)
        print(f"[bench]  [warmup done in {(time.monotonic()-t0)*1000:.1f} ms]", file=sys.stderr)
    rows = []
    for i in range(iters):
        print(f"[bench]  iter {i+1}/{iters}", file=sys.stderr)
        wall_ms, stdout = run_once(cmd, cwd)
        timings = parse_timings(stdout)
        if len(timings) != expected_timings:
            print(
                f"[bench]  WARNING: expected {expected_timings} COMPRESSION_MS lines, "
                f"got {len(timings)}",
                file=sys.stderr,
            )
        for d_idx, size in enumerate(sizes):
            for it in range(inner_iters):
                timing_idx = d_idx * inner_iters + it
                compression_ms = timings[timing_idx] if timing_idx < len(timings) else None
                rows.append(
                    dict(
                        backend=backend,
                        size_bytes=size,
                        level=level,
                        iter=i,
                        inner_iter=it,
                        wall_ms=wall_ms / expected_timings if expected_timings else wall_ms,
                        compression_ms=compression_ms,
                    )
                )
        preview = timings[:4]
        suffix = "..." if len(timings) > 4 else ""
        print(
            f"[bench]  iter {i+1} done: wall_ms={wall_ms:.1f}, timings={preview}{suffix}",
            file=sys.stderr,
        )
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument(
        "--library", choices=[lib.value for lib in Library], default=Library.JPEG.value,
        help="library to benchmark (default: jpeg).  "
             "For zlib, levels are compression levels 1-9 (higher = more compression); "
             "for jpeg, levels are quality values 1-100 (higher = LESS compression).",
    )
    ap.add_argument("--wasm2c-build-dir", type=Path, default=None,
                    help="build dir holding the wasm2c `main` binary and bench_native "
                         "(default: test/<library>-testing/build)")
    ap.add_argument("--sizes", type=str, default="100k, 250k, 500k",
                    help="input sizes, comma-separated; k/m suffixes ok.  Each size "
                         "becomes one numbered dataset (test_data/test_data{N}.txt) "
                         "and all datasets are processed in a single binary invocation.")
    ap.add_argument("--levels", type=str, default=None,
                    help="compression levels (zlib: 1-9, higher = more compression) or "
                         "quality values (jpeg: 1-100, higher = LESS compression). "
                         "Comma-separated. Defaults: zlib -> 1,6,9; jpeg -> 90,50,25,10.")
    ap.add_argument("--iters", type=int, default=3,
                    help="outer iterations per (backend, level): number of times the "
                         "entire binary is re-invoked (statistical repetitions)")
    ap.add_argument("--num-datasets", type=int, default=None,
                    help="number of datasets passed to each binary as the second "
                         "positional arg (default: len(sizes)).  Must not exceed len(sizes).")
    ap.add_argument("--inner-iters", type=int, default=10,
                    help="inner iterations per dataset inside the binary, passed as "
                         "the third positional arg (default: 1)")
    ap.add_argument("--out", type=Path, default=BENCH_DIR / "results.csv")
    ap.add_argument("--no-wasm2c", action="store_true",
                    help="skip the wasm2c backend (e.g. if not built)")
    ap.add_argument("--no-process", action="store_true",
                    help="skip the process backends entirely")
    ap.add_argument("--no-adaptive", action="store_true",
                    help="skip the adaptive backend (build/main_adaptive)")
    args = ap.parse_args()

    # --- Per-library config ---------------------------------------------------
    library = Library(args.library)
    testing_dir = library.testing_dir
    default_levels = library.default_levels

    wasm2c_build_dir = (
        args.wasm2c_build_dir.resolve()
        if args.wasm2c_build_dir is not None
        else testing_dir / "build"
    )
    sizes = parse_sizes(args.sizes)
    num_datasets = min(args.num_datasets, len(sizes)) if args.num_datasets is not None else len(sizes)
    inner_iters = args.inner_iters
    levels = parse_ints(args.levels if args.levels is not None else default_levels)
    source_data = testing_dir / "test_data.txt"
    # -------------------------------------------------------------------------

    # --- Validate backends ---------------------------------------------------
    main_bin = wasm2c_build_dir / "main"
    if not args.no_wasm2c and not main_bin.exists():
        print(f"[bench] {main_bin} missing; pass --no-wasm2c or rebuild",
              file=sys.stderr)
        return 1

    process_build_dir: Optional[Path] = None
    if not args.no_process:
        process_build_dir = testing_dir / "build"
        if not (process_build_dir / "main_process").exists():
            print(f"[bench] {process_build_dir}/main_process missing; configure "
                  f"with -DRLBOX_TRANSPORT=... and rebuild",
                  file=sys.stderr)
            return 1
        if not (process_build_dir / "sandbox_shim.so").exists():
            print(f"[bench] {process_build_dir}/sandbox_shim.so missing",
                  file=sys.stderr)
            return 1

    adaptive_build_dir: Optional[Path] = None
    if not args.no_adaptive:
        adaptive_build_dir = testing_dir / "build"
        if not (adaptive_build_dir / "main_adaptive").exists():
            print(f"[bench] {adaptive_build_dir}/main_adaptive missing; "
                  f"pass --no-adaptive or rebuild",
                  file=sys.stderr)
            return 1
        if not (adaptive_build_dir / "sandbox_shim.so").exists():
            print(f"[bench] {adaptive_build_dir}/sandbox_shim.so missing",
                  file=sys.stderr)
            return 1

    bench_native = ensure_bench_native(wasm2c_build_dir)
    # -------------------------------------------------------------------------

    # wasm2c_build_dir is always included so bench_native has a test_data/ dir.
    # Deduplicate while preserving order.
    write_dirs: list[Path] = [wasm2c_build_dir]
    if process_build_dir is not None:
        write_dirs.append(process_build_dir)
    if adaptive_build_dir is not None:
        write_dirs.append(adaptive_build_dir)
    seen: set[Path] = set()
    write_dirs = [d for d in write_dirs if not (d in seen or seen.add(d))]  # type: ignore[func-returns-value]

    seed_bytes = source_data.read_bytes()

    # Backup test_data/ directories so we can restore them afterwards.
    backups: list[tuple[Path, Optional[Path]]] = []
    for d in write_dirs:
        data_dir = d / "test_data"
        if data_dir.exists():
            backup = d / "test_data_bench_backup"
            shutil.copytree(str(data_dir), str(backup))
            backups.append((data_dir, backup))
        else:
            backups.append((data_dir, None))
    print(f"[bench] will write test_data/ in {len(write_dirs)} build dir(s)",
          file=sys.stderr)

    # Sizes used in this run (may be fewer than len(sizes) if --num-datasets set).
    active_sizes = sizes[:num_datasets]
    # Positional args appended to every binary invocation after the quality/level.
    cmd_suffix = [str(num_datasets), str(inner_iters)]

    all_rows: list[dict] = []
    try:
        print(f"[bench] preparing {num_datasets} dataset(s) in each build dir", file=sys.stderr)
        t0 = time.monotonic()
        for d in write_dirs:
            prepare_inputs_dir(d, seed_bytes, active_sizes, library=library)
        print(f"[bench] input preparation done in {(time.monotonic()-t0)*1000:.1f} ms",
              file=sys.stderr)

        for level in levels:
            print(f"[bench] level={level}", file=sys.stderr)

            if process_build_dir is not None:
                rows = run_config_multi(
                    "process",
                    [str(process_build_dir / "main_process"), str(level)] + cmd_suffix,
                    cwd=process_build_dir,
                    sizes=active_sizes,
                    level=level,
                    inner_iters=inner_iters,
                    iters=args.iters,
                    warmup=True,
                )
                all_rows.extend(rows)

            if adaptive_build_dir is not None:
                rows = run_config_multi(
                    "adaptive",
                    [str(adaptive_build_dir / "main_adaptive"), str(level)] + cmd_suffix,
                    cwd=adaptive_build_dir,
                    sizes=active_sizes,
                    level=level,
                    inner_iters=inner_iters,
                    iters=args.iters,
                    warmup=True,
                )
                all_rows.extend(rows)

            # Native reference always runs from wasm2c_build_dir.
            rows = run_config_multi(
                "native",
                [str(bench_native), str(level)] + cmd_suffix,
                cwd=wasm2c_build_dir,
                sizes=active_sizes,
                level=level,
                inner_iters=inner_iters,
                iters=args.iters,
                warmup=True,
            )
            all_rows.extend(rows)

            if not args.no_wasm2c:
                rows = run_config_multi(
                    "wasm2c",
                    [str(main_bin), str(level)] + cmd_suffix,
                    cwd=wasm2c_build_dir,
                    sizes=active_sizes,
                    level=level,
                    inner_iters=inner_iters,
                    iters=args.iters,
                    warmup=True,
                )
                all_rows.extend(rows)
    finally:
        for data_dir, backup in backups:
            shutil.rmtree(str(data_dir), ignore_errors=True)
            if backup is not None:
                shutil.move(str(backup), str(data_dir))
        print("[bench] restored test_data/ directories", file=sys.stderr)

    # Write CSV.
    fields = ["backend", "size_bytes", "level", "iter", "inner_iter", "wall_ms", "compression_ms"]
    with open(args.out, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=fields)
        w.writeheader()
        w.writerows(all_rows)
    print(f"[bench] wrote {len(all_rows)} rows -> {args.out}", file=sys.stderr)

    # Short summary: median compression_ms (or wall_ms) per (backend, size, level),
    # aggregated over all inner_iter and iter values.
    # compression_ms may be None when the binary doesn't print COMPRESSION_MS=;
    # fall back to wall_ms in that case.
    def key(r):
        return (r["backend"], r["size_bytes"], r["level"])

    by_key: dict = {}
    for r in all_rows:
        by_key.setdefault(key(r), []).append(r)

    use_compression_ms = any(r["compression_ms"] is not None for r in all_rows)
    metric_label = "median_compression_ms" if use_compression_ms else "median_wall_ms  "
    print(f"\nbackend          size       level   {metric_label}", file=sys.stderr)
    print("-" * 65, file=sys.stderr)
    for k in sorted(by_key.keys()):
        rows = by_key[k]
        backend, size_bytes, level = k
        if use_compression_ms:
            vals = [r["compression_ms"] for r in rows if r["compression_ms"] is not None]
            t = statistics.median(vals) if vals else float("nan")
        else:
            t = statistics.median(r["wall_ms"] for r in rows)
        print(f"{backend:<16} {size_bytes:<10} {level:<7} {t:>10.2f}",
              file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
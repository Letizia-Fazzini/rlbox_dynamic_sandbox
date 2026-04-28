#!/usr/bin/env python3
"""
Drive the benchmark across 4 backends for a chosen library, expecting THREE
separate build directories instead of one shared build/:

    - native:          build_wasm/bench_native       (no sandboxing at all)
    - wasm2c:          build_wasm/main               (RLBox wasm2c sandbox)
    - process_rpclib:  build_rpclib/main_process     (RLBox process sandbox, rpclib transport)
    - process_capnp:   build_capnp/main_process      (RLBox process sandbox, capnp transport)

Select the library with --library {zlib,jpeg} (default: jpeg).

For zlib: compression levels are 1-9 (higher = more compression).
For jpeg: quality levels are 1-100 (higher = LESS compression / better quality).
  Recommended quality values: 90, 50, 25, 10.

Each (backend, level, size) combination is measured in its OWN binary
invocation: the binary is always called with num_datasets=1 and inner_iters=1,
so the nested loops in main.cpp run exactly once per call and each file size
is its own independent test case.  Outer --iters still controls statistical
repetitions.

Each binary prints lines like
    COMPRESSION_MS=12.345
exactly once per invocation under this script (one dataset x one inner iter).

All binaries accept: binary quality [num_datasets] [inner_iters]
and read from test_data/test_data{d}.txt (relative to cwd).  This script
creates that directory in each build dir, writes test_data1.txt sized to the
current size before each backend invocation, then restores any pre-existing
test_data/ directories afterwards.
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
from pathlib import Path
from typing import Optional

BENCH_DIR = Path(__file__).resolve().parent
REPO_ROOT = BENCH_DIR.parent

INNER_ITERS = 1
NUM_DATASETS = 1


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
        raise RuntimeError("empty source bytes — can't repeat to target size")

    nl = source_bytes.find(b"\n")
    if nl == -1:
        raise RuntimeError("invalid data files - no header")
    else:
        header = source_bytes[:nl]
        body = source_bytes[nl + 1:]

    if library is Library.ZLIB:
        if not body:
            raise RuntimeError("invalid data files - empty body")
        header_part = (header + b"\n")
        ratio = size / len(body)
        full_reps = int(ratio)
        extra_bytes = round((ratio - full_reps) * len(body))
        blob = header_part + body * full_reps + body[:extra_bytes]
        target.write_bytes(blob)

    elif library is Library.JPEG:
        W, H, C = map(int, header.split())
        body_lines = [line for line in body.split(b"\n") if line]
        if not body_lines:
            raise RuntimeError("JPEG test_data.txt has no body lines to repeat")

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


def write_single_input(build_dir: Path, source_bytes: bytes, size: int, library: Library) -> None:
    """Write build_dir/test_data/test_data1.txt sized to `size`."""
    data_dir = build_dir / "test_data"
    data_dir.mkdir(parents=True, exist_ok=True)
    _write_input(data_dir / "test_data1.txt", source_bytes, size, library=library)


def ensure_bench_native(build_dir: Path) -> Path:
    binary = build_dir / "bench_native"
    if not binary.exists():
        print(f"[bench] {binary} missing; rebuild with cmake", file=sys.stderr)
        sys.exit(1)
    return binary


def run_once(
    cmd: list[str],
    cwd: Path,
    backend: str,
    level: int,
    size: int,
) -> str:
    """Run cmd under cwd, return stdout_text.

    Always runs with num_datasets=1 and inner_iters=1; one stderr announcement
    per call.
    """
    print(
        f"[bench] backend={backend} level={level} size={size} "
        f"inner_iters={INNER_ITERS}",
        file=sys.stderr,
    )
    res = subprocess.run(
        cmd, cwd=str(cwd), capture_output=True, text=True
    )
    if res.returncode != 0:
        raise subprocess.CalledProcessError(
            res.returncode, cmd,
            output=res.stdout, stderr=res.stderr,
        )
    return res.stdout


def parse_timings(stdout: str) -> list[float]:
    """Return all COMPRESSION_MS= values found in stdout, in order."""
    return [float(v) for v in COMPRESSION_RE.findall(stdout)]


def run_config_single(
    backend: str,
    cmd: list[str],
    cwd: Path,
    size: int,
    level: int,
    iters: int,
    warmup: bool,
) -> list[dict]:
    """Run `cmd` `iters` times for a single (backend, level, size) cell.

    The binary is always called with num_datasets=1 and inner_iters=1, so each
    invocation must produce exactly one COMPRESSION_MS= line.
    """
    if warmup:
        run_once(cmd, cwd, backend, level, size)
    rows = []
    for i in range(iters):
        stdout = run_once(cmd, cwd, backend, level, size)
        timings = parse_timings(stdout)
        if len(timings) != 1:
            raise RuntimeError(
                f"backend={backend} level={level} size={size} iter={i}: "
                f"expected 1 COMPRESSION_MS line, got {len(timings)}"
            )
        rows.append(
            dict(
                backend=backend,
                level=level,
                size=size,
                iter=i,
                inner_iter=0,
                compression_ms=timings[0],
            )
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
    ap.add_argument("--wasm-build-dir", type=Path, default=None,
                    help="build dir holding `main` (wasm2c) and `bench_native` "
                         "(default: test/<library>-testing/build_wasm)")
    ap.add_argument("--rpclib-build-dir", type=Path, default=None,
                    help="build dir holding `main_process` built with the rpclib "
                         "transport (default: test/<library>-testing/build_rpclib)")
    ap.add_argument("--capnp-build-dir", type=Path, default=None,
                    help="build dir holding `main_process` built with the capnp "
                         "transport (default: test/<library>-testing/build_capnp)")
    ap.add_argument("--sizes", type=str, default="5k, 250k",
                    help="input sizes, comma-separated; k/m suffixes ok.  Each "
                         "size is benchmarked in its own binary invocation per "
                         "backend/level/iter (num_datasets=1, inner_iters=1).")
    ap.add_argument("--levels", type=str, default=None,
                    help="compression levels (zlib: 1-9, higher = more compression) or "
                         "quality values (jpeg: 1-100, higher = LESS compression). "
                         "Comma-separated. Defaults: zlib -> 1,6,9; jpeg -> 90,50,25,10.")
    ap.add_argument("--iters", type=int, default=3,
                    help="outer iterations per (backend, level, size): number of "
                         "times the entire binary is re-invoked (statistical "
                         "repetitions)")
    ap.add_argument("--out", type=Path, default=BENCH_DIR / "results.csv")
    ap.add_argument("--no-wasm2c", action="store_true",
                    help="skip the wasm2c backend (e.g. if not built)")
    ap.add_argument("--no-rpclib", action="store_true",
                    help="skip the process_rpclib backend")
    ap.add_argument("--no-capnp", action="store_true",
                    help="skip the process_capnp backend")
    args = ap.parse_args()

    # --- Per-library config ---------------------------------------------------
    library = Library(args.library)
    testing_dir = library.testing_dir
    default_levels = library.default_levels

    wasm_build_dir = (
        args.wasm_build_dir.resolve()
        if args.wasm_build_dir is not None
        else testing_dir / "build_wasm"
    )
    rpclib_build_dir = (
        args.rpclib_build_dir.resolve()
        if args.rpclib_build_dir is not None
        else testing_dir / "build_rpclib"
    )
    capnp_build_dir = (
        args.capnp_build_dir.resolve()
        if args.capnp_build_dir is not None
        else testing_dir / "build_capnp"
    )
    sizes = parse_sizes(args.sizes)
    levels = parse_ints(args.levels if args.levels is not None else default_levels)
    source_data = testing_dir / "test_data.txt"
    # -------------------------------------------------------------------------

    # --- Validate backends ---------------------------------------------------
    bench_native = ensure_bench_native(wasm_build_dir)

    main_bin = wasm_build_dir / "main"
    if not args.no_wasm2c and not main_bin.exists():
        print(f"[bench] {main_bin} missing; pass --no-wasm2c or rebuild",
              file=sys.stderr)
        return 1

    if not args.no_rpclib:
        if not (rpclib_build_dir / "main_process").exists():
            print(f"[bench] {rpclib_build_dir}/main_process missing; configure "
                  f"with -DRLBOX_TRANSPORT=rpclib and rebuild, or pass --no-rpclib",
                  file=sys.stderr)
            return 1
        if not (rpclib_build_dir / "sandbox_shim.so").exists():
            print(f"[bench] {rpclib_build_dir}/sandbox_shim.so missing",
                  file=sys.stderr)
            return 1

    if not args.no_capnp:
        if not (capnp_build_dir / "main_process").exists():
            print(f"[bench] {capnp_build_dir}/main_process missing; configure "
                  f"with -DRLBOX_TRANSPORT=capnp and rebuild, or pass --no-capnp",
                  file=sys.stderr)
            return 1
        if not (capnp_build_dir / "sandbox_shim.so").exists():
            print(f"[bench] {capnp_build_dir}/sandbox_shim.so missing",
                  file=sys.stderr)
            return 1
    # -------------------------------------------------------------------------

    # All build dirs that need a test_data/ directory written.  wasm_build_dir
    # is always present (bench_native lives there).  Deduplicate while
    # preserving order in case the user points two flags at the same dir.
    write_dirs: list[Path] = [wasm_build_dir]
    if not args.no_rpclib:
        write_dirs.append(rpclib_build_dir)
    if not args.no_capnp:
        write_dirs.append(capnp_build_dir)
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

    # Suffix appended to every binary call: num_datasets inner_iters (both 1).
    cmd_suffix = [str(NUM_DATASETS), str(INNER_ITERS)]

    # warmup is per (backend, level): once we've warmed a (backend, level)
    # pair it stays warm across the inner size loop.
    warmed: set[tuple[str, int]] = set()

    all_rows: list[dict] = []
    try:
        for level in levels:
            for size in sizes:
                # Refresh test_data1.txt at this size in every active build dir.
                for d in write_dirs:
                    write_single_input(d, seed_bytes, size, library=library)

                # Fixed backend order: process backends first, then native, then wasm2c.
                if not args.no_rpclib:
                    backend = "process_rpclib"
                    do_warm = (backend, level) not in warmed
                    rows = run_config_single(
                        backend,
                        [str(rpclib_build_dir / "main_process"), str(level)] + cmd_suffix,
                        cwd=rpclib_build_dir,
                        size=size,
                        level=level,
                        iters=args.iters,
                        warmup=do_warm,
                    )
                    warmed.add((backend, level))
                    all_rows.extend(rows)

                if not args.no_capnp:
                    backend = "process_capnp"
                    do_warm = (backend, level) not in warmed
                    rows = run_config_single(
                        backend,
                        [str(capnp_build_dir / "main_process"), str(level)] + cmd_suffix,
                        cwd=capnp_build_dir,
                        size=size,
                        level=level,
                        iters=args.iters,
                        warmup=do_warm,
                    )
                    warmed.add((backend, level))
                    all_rows.extend(rows)

                # Native always available (bench_native presence is checked above).
                backend = "native"
                do_warm = (backend, level) not in warmed
                rows = run_config_single(
                    backend,
                    [str(bench_native), str(level)] + cmd_suffix,
                    cwd=wasm_build_dir,
                    size=size,
                    level=level,
                    iters=args.iters,
                    warmup=do_warm,
                )
                warmed.add((backend, level))
                all_rows.extend(rows)

                if not args.no_wasm2c:
                    backend = "wasm2c"
                    do_warm = (backend, level) not in warmed
                    rows = run_config_single(
                        backend,
                        [str(main_bin), str(level)] + cmd_suffix,
                        cwd=wasm_build_dir,
                        size=size,
                        level=level,
                        iters=args.iters,
                        warmup=do_warm,
                    )
                    warmed.add((backend, level))
                    all_rows.extend(rows)
    finally:
        for data_dir, backup in backups:
            shutil.rmtree(str(data_dir), ignore_errors=True)
            if backup is not None:
                shutil.move(str(backup), str(data_dir))

    # Write CSV (schema unchanged from run_adaptive_bench.py).
    fields = ["backend", "level", "size", "iter", "inner_iter", "compression_ms"]
    with open(args.out, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=fields)
        w.writeheader()
        w.writerows(all_rows)
    print(f"[bench] wrote {len(all_rows)} rows -> {args.out}", file=sys.stderr)

    # Short summary: median compression_ms per (backend, level, size).
    def key(r):
        return (r["backend"], r["level"], r["size"])

    by_key: dict = {}
    for r in all_rows:
        by_key.setdefault(key(r), []).append(r)

    print(f"\nbackend          level   size         median_compression_ms", file=sys.stderr)
    print("-" * 64, file=sys.stderr)
    for k in sorted(by_key.keys()):
        rows = by_key[k]
        backend, level, size = k
        t = statistics.median(r["compression_ms"] for r in rows)
        print(f"{backend:<16} {level:<7} {size:<12} {t:>10.2f}",
              file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

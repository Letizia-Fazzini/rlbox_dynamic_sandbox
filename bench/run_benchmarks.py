#!/usr/bin/env python3
"""
Drive the zlib benchmark across three backends:
    - native:  bench_native (stock libz)
    - wasm2c:  test/zlib-testing/build/main (RLBox wasm2c sandbox)
    - process: test/zlib-testing/build/main_process (this repo's process sandbox)

Each backend's main runs the zlib compression loop and prints a line like
    SANDBOX_MS=12.345 NATIVE_MS=11.222
(for the sandbox mains) or
    NATIVE_MS=11.222
(for bench_native). We parse that, plus the Python wall-clock wrapper, and
record everything to a CSV for plotting.

The sandbox mains hard-code 'pi.txt' as the input file and './sandbox_shim.so'
as the shim preload path. So we run each binary from the build directory and
stage inputs by overwriting pi.txt there, restoring the original at the end.
"""
from __future__ import annotations

import argparse
import csv
import os
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
DEFAULT_BUILD_DIR = REPO_ROOT / "test" / "zlib-testing" / "build"

SANDBOX_RE = re.compile(r"SANDBOX_MS=([\d.]+)")
NATIVE_RE = re.compile(r"NATIVE_MS=([\d.]+)")


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


def ensure_bench_native() -> Path:
    """Build bench_native if the binary is missing or stale."""
    binary = BENCH_DIR / "bench_native"
    src = BENCH_DIR / "bench_native.c"
    if not binary.exists() or binary.stat().st_mtime < src.stat().st_mtime:
        print("[bench] building bench_native...", file=sys.stderr)
        subprocess.run(
            ["make", "-C", str(BENCH_DIR), "bench_native"], check=True
        )
    return binary


def prepare_input(build_dir: Path, source_bytes: bytes, size: int) -> None:
    """Write exactly `size` bytes to build_dir/pi.txt by repeating source_bytes."""
    target = build_dir / "pi.txt"
    if not source_bytes:
        raise RuntimeError("empty source bytes — can't repeat to target size")
    reps = (size + len(source_bytes) - 1) // len(source_bytes)
    blob = (source_bytes * reps)[:size]
    target.write_bytes(blob)


def run_once(cmd: list[str], cwd: Path) -> tuple[float, str]:
    """Run cmd under cwd, return (wall_ms, stdout_text)."""
    t0 = time.monotonic()
    res = subprocess.run(
        cmd, cwd=str(cwd), capture_output=True, text=True, check=True
    )
    wall_ms = (time.monotonic() - t0) * 1000.0
    return wall_ms, res.stdout


def parse_timing(stdout: str) -> tuple[Optional[float], Optional[float]]:
    sm = SANDBOX_RE.search(stdout)
    nm = NATIVE_RE.search(stdout)
    return (
        float(sm.group(1)) if sm else None,
        float(nm.group(1)) if nm else None,
    )


def run_config(
    backend: str,
    cmd: list[str],
    cwd: Path,
    size: int,
    level: int,
    iters: int,
    warmup: bool,
) -> list[dict]:
    """Run `cmd` `iters` times and return a list of records."""
    if warmup:
        run_once(cmd, cwd)
    rows = []
    for i in range(iters):
        wall_ms, stdout = run_once(cmd, cwd)
        sandbox_ms, native_ms = parse_timing(stdout)
        rows.append(
            dict(
                backend=backend,
                size_bytes=size,
                level=level,
                iter=i,
                wall_ms=wall_ms,
                sandbox_ms=sandbox_ms,
                native_ms=native_ms,
            )
        )
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR,
                    help="zlib-testing build directory (default: %(default)s)")
    ap.add_argument("--sizes", type=str, default="256k,1m,4m,16m",
                    help="input sizes, comma-separated; k/m suffixes ok")
    ap.add_argument("--levels", type=str, default="1,6,9",
                    help="compression levels, comma-separated")
    ap.add_argument("--iters", type=int, default=3,
                    help="iterations per (backend, size, level)")
    ap.add_argument("--out", type=Path, default=BENCH_DIR / "results.csv")
    ap.add_argument("--no-wasm2c", action="store_true",
                    help="skip the wasm2c backend (e.g. if not built)")
    ap.add_argument("--no-process", action="store_true",
                    help="skip the process backend")
    args = ap.parse_args()

    build_dir = args.build_dir.resolve()
    sizes = parse_sizes(args.sizes)
    levels = parse_ints(args.levels)

    main_bin = build_dir / "main"
    main_process_bin = build_dir / "main_process"
    pi_txt = build_dir / "pi.txt"
    shim = build_dir / "sandbox_shim.so"

    if not args.no_wasm2c and not main_bin.exists():
        print(f"[bench] {main_bin} missing; pass --no-wasm2c or rebuild", file=sys.stderr)
        return 1
    if not args.no_process:
        if not main_process_bin.exists():
            print(f"[bench] {main_process_bin} missing; pass --no-process or rebuild", file=sys.stderr)
            return 1
        if not shim.exists():
            print(f"[bench] {shim} missing — main_process needs it in CWD", file=sys.stderr)
            return 1
    if not pi_txt.exists():
        print(f"[bench] {pi_txt} missing; cannot seed inputs", file=sys.stderr)
        return 1

    bench_native = ensure_bench_native()

    # Save original pi.txt so we can restore it after benchmarking.
    source_bytes = pi_txt.read_bytes()
    backup = pi_txt.with_suffix(".txt.bench_backup")
    shutil.copyfile(pi_txt, backup)
    print(f"[bench] backed up pi.txt -> {backup.name}", file=sys.stderr)

    all_rows: list[dict] = []
    try:
        for size in sizes:
            prepare_input(build_dir, source_bytes, size)
            for level in levels:
                print(f"[bench] size={size} level={level}", file=sys.stderr)

                # Native reference — just needs the path to pi.txt.
                rows = run_config(
                    "native",
                    [str(bench_native), str(pi_txt), str(level)],
                    cwd=build_dir,
                    size=size,
                    level=level,
                    iters=args.iters,
                    warmup=True,
                )
                all_rows.extend(rows)

                if not args.no_wasm2c:
                    rows = run_config(
                        "wasm2c",
                        [str(main_bin), str(level)],
                        cwd=build_dir,
                        size=size,
                        level=level,
                        iters=args.iters,
                        warmup=True,
                    )
                    all_rows.extend(rows)

                if not args.no_process:
                    rows = run_config(
                        "process",
                        [str(main_process_bin), str(level)],
                        cwd=build_dir,
                        size=size,
                        level=level,
                        iters=args.iters,
                        warmup=True,
                    )
                    all_rows.extend(rows)
    finally:
        shutil.move(str(backup), str(pi_txt))
        print("[bench] restored pi.txt", file=sys.stderr)

    # Write CSV.
    fields = ["backend", "size_bytes", "level", "iter", "wall_ms", "sandbox_ms", "native_ms"]
    with open(args.out, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=fields)
        w.writeheader()
        w.writerows(all_rows)
    print(f"[bench] wrote {len(all_rows)} rows -> {args.out}", file=sys.stderr)

    # Short summary: median elapsed time per (backend, size, level).
    def key(r):
        return (r["backend"], r["size_bytes"], r["level"])

    by_key: dict = {}
    for r in all_rows:
        by_key.setdefault(key(r), []).append(r)

    print("\nbackend   size       level   metric        median_ms", file=sys.stderr)
    print("-" * 64, file=sys.stderr)
    for k in sorted(by_key.keys()):
        rows = by_key[k]
        backend, size_bytes, level = k
        if backend == "native":
            t = statistics.median(r["native_ms"] for r in rows)
            metric = "native"
        else:
            t = statistics.median(r["sandbox_ms"] for r in rows)
            metric = "sandbox"
        print(f"{backend:<9} {size_bytes:<10} {level:<7} {metric:<12} {t:>10.2f}",
              file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

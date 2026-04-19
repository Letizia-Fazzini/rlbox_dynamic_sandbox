#!/usr/bin/env python3
"""
Drive the zlib benchmark across three backends:
    - native:  bench_native (stock libz)
    - wasm2c:  test/zlib-testing/build/main (RLBox wasm2c sandbox)
    - process: test/zlib-testing/build/main_process (this repo's process sandbox)

Each binary runs the zlib compression loop and prints a line like
    COMPRESSION_MS=12.345
We parse that, plus the Python wall-clock wrapper, and record everything to a
CSV for plotting.

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
ZLIB_TESTING_DIR = REPO_ROOT / "test" / "zlib-testing"
DEFAULT_BUILD_DIR = ZLIB_TESTING_DIR / "build"

# Process-backend variants we'll drive when --process-builds is left at the
# default.  Each entry is (label, build-subdir-name); the build dir defaults
# to test/zlib-testing/<subdir>.  Adding a new transport is one line here +
# a corresponding cmake build dir.
DEFAULT_PROCESS_BUILDS = [
    ("process_rpclib", "build_rpclib"),
    ("process_capnp", "build_capnp"),
]

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


def parse_timing(stdout: str) -> Optional[float]:
    m = COMPRESSION_RE.search(stdout)
    return float(m.group(1)) if m else None


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
        compression_ms = parse_timing(stdout)
        rows.append(
            dict(
                backend=backend,
                size_bytes=size,
                level=level,
                iter=i,
                wall_ms=wall_ms,
                compression_ms=compression_ms,
            )
        )
    return rows


def parse_process_builds(s: Optional[str]) -> list[tuple[str, Path]]:
    """Parse a comma-separated list of label:path entries.  An entry without
    a colon is treated as `process_<label>:<zlib-testing>/<label>`.
    """
    if not s:
        return [
            (label, ZLIB_TESTING_DIR / subdir)
            for (label, subdir) in DEFAULT_PROCESS_BUILDS
        ]
    out: list[tuple[str, Path]] = []
    for chunk in s.split(","):
        c = chunk.strip()
        if not c:
            continue
        if ":" in c:
            label, path = c.split(":", 1)
            out.append((label.strip(), Path(path.strip()).resolve()))
        else:
            out.append((f"process_{c}", (ZLIB_TESTING_DIR / c).resolve()))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--wasm2c-build-dir", type=Path, default=DEFAULT_BUILD_DIR,
                    help="build dir holding the wasm2c `main` binary "
                         "(default: %(default)s)")
    ap.add_argument("--process-builds", type=str, default=None,
                    help="comma-separated process variants to drive, "
                         "either `label:path` or `subdir` (resolved under "
                         "test/zlib-testing).  Default: rpclib + capnp.")
    ap.add_argument("--sizes", type=str, default="256k,1m,4m",
                    help="input sizes, comma-separated; k/m suffixes ok")
    ap.add_argument("--levels", type=str, default="1,6,9",
                    help="compression levels, comma-separated")
    ap.add_argument("--iters", type=int, default=3,
                    help="iterations per (backend, size, level)")
    ap.add_argument("--out", type=Path, default=BENCH_DIR / "results.csv")
    ap.add_argument("--no-wasm2c", action="store_true",
                    help="skip the wasm2c backend (e.g. if not built)")
    ap.add_argument("--no-process", action="store_true",
                    help="skip the process backends entirely")
    args = ap.parse_args()

    wasm2c_build_dir = args.wasm2c_build_dir.resolve()
    sizes = parse_sizes(args.sizes)
    levels = parse_ints(args.levels)

    main_bin = wasm2c_build_dir / "main"
    if not args.no_wasm2c and not main_bin.exists():
        print(f"[bench] {main_bin} missing; pass --no-wasm2c or rebuild",
              file=sys.stderr)
        return 1
    # Native + wasm2c read pi.txt from the wasm2c build dir; we still need
    # *some* seed file even when wasm2c is skipped, so fall back to whichever
    # process build we'll use.
    seed_dir = wasm2c_build_dir if not args.no_wasm2c else None

    process_builds: list[tuple[str, Path]] = []
    if not args.no_process:
        process_builds = parse_process_builds(args.process_builds)
        for label, build_dir in process_builds:
            if not (build_dir / "main_process").exists():
                print(f"[bench] {build_dir}/main_process missing; configure "
                      f"with -DRLBOX_TRANSPORT=... and rebuild",
                      file=sys.stderr)
                return 1
            if not (build_dir / "sandbox_shim.so").exists():
                print(f"[bench] {build_dir}/sandbox_shim.so missing",
                      file=sys.stderr)
                return 1
            if not (build_dir / "pi.txt").exists():
                print(f"[bench] {build_dir}/pi.txt missing; cannot seed",
                      file=sys.stderr)
                return 1
        if seed_dir is None:
            seed_dir = process_builds[0][1]
    if seed_dir is None:
        print("[bench] nothing to do — all backends disabled", file=sys.stderr)
        return 1

    bench_native = ensure_bench_native(wasm2c_build_dir)

    # Save the original pi.txt for each build dir we'll be writing to so we
    # can restore them all after benchmarking.
    write_dirs = []
    if not args.no_wasm2c:
        write_dirs.append(wasm2c_build_dir)
    write_dirs.extend(d for _, d in process_builds)
    backups: list[tuple[Path, Path]] = []
    seed_bytes: Optional[bytes] = None
    for d in write_dirs:
        pi = d / "pi.txt"
        if seed_bytes is None:
            seed_bytes = pi.read_bytes()
        backup = pi.with_suffix(".txt.bench_backup")
        shutil.copyfile(pi, backup)
        backups.append((pi, backup))
    print(f"[bench] backed up pi.txt in {len(backups)} build dir(s)",
          file=sys.stderr)

    all_rows: list[dict] = []
    try:
        for size in sizes:
            for d in write_dirs:
                prepare_input(d, seed_bytes, size)
            for level in levels:
                print(f"[bench] size={size} level={level}", file=sys.stderr)

                # Native reference — runs from whichever dir we picked for
                # seeding (pi.txt is identical across them).
                native_pi = (write_dirs[0] / "pi.txt")
                rows = run_config(
                    "native",
                    [str(bench_native), str(native_pi), str(level)],
                    cwd=write_dirs[0],
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
                        cwd=wasm2c_build_dir,
                        size=size,
                        level=level,
                        iters=args.iters,
                        warmup=True,
                    )
                    all_rows.extend(rows)

                for label, build_dir in process_builds:
                    rows = run_config(
                        label,
                        [str(build_dir / "main_process"), str(level)],
                        cwd=build_dir,
                        size=size,
                        level=level,
                        iters=args.iters,
                        warmup=True,
                    )
                    all_rows.extend(rows)
    finally:
        for pi, backup in backups:
            shutil.move(str(backup), str(pi))
        print("[bench] restored pi.txt files", file=sys.stderr)

    # Write CSV.
    fields = ["backend", "size_bytes", "level", "iter", "wall_ms", "compression_ms"]
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

    print("\nbackend          size       level   median_compression_ms", file=sys.stderr)
    print("-" * 60, file=sys.stderr)
    for k in sorted(by_key.keys()):
        rows = by_key[k]
        backend, size_bytes, level = k
        t = statistics.median(r["compression_ms"] for r in rows)
        print(f"{backend:<16} {size_bytes:<10} {level:<7} {t:>10.2f}",
              file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

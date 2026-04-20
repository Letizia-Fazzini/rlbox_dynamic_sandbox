#!/usr/bin/env python3
"""
Drive the benchmark across three backends for a chosen library:
    - native:  bench_native (stock libz or libjpeg)
    - wasm2c:  test/<lib>-testing/build/main (RLBox wasm2c sandbox)
    - process: test/<lib>-testing/build/main_process (this repo's process sandbox)

Select the library with --library {zlib,jpeg} (default: jpeg).

For zlib: compression levels are 1-9 (higher = more compression).
For jpeg: quality levels are 1-100 (higher = LESS compression / better quality).
  Recommended quality values: 90, 50, 25, 10.

Each binary may print a line like
    COMPRESSION_MS=12.345
which is parsed as the in-binary timing; we also record Python wall-clock time.

The sandbox mains hard-code 'test_data.txt' as the input file and './sandbox_shim.so'
as the shim preload path. So we run each binary from the build directory and
stage inputs by overwriting test_data.txt there, restoring the original at the end.

Both test_data.txt files have a header line that must appear exactly once; only
the body is repeated to reach the target size.  For libjpeg the header encodes
image dimensions (W H C) and is rewritten to reflect the actual number of rows
included, so the C binaries always read a consistent file.
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


def prepare_input(build_dir: Path, source_bytes: bytes, size: int, library: Library = Library.ZLIB) -> None:
    """Write a resized test_data.txt to build_dir.

    The first line of source_bytes (the header) is written exactly once.  Only
    the body is repeated to reach approximately `size` bytes.

    For zlib, body bytes are repeated at the byte level.
    For jpeg, body lines are repeated at the line level and the header W/H/C
    line is rewritten with the actual number of rows included, so that the C
    binaries (which fscanf the header) always read a consistent file.
    """
    target = build_dir / "test_data.txt"
    if not source_bytes:
        raise RuntimeError("empty source bytes — can't repeat to target size")

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
    print(f"[bench] backend={backend} size={size} level={level} warmup={warmup} iters={iters}",
          file=sys.stderr)
    if warmup:
        print(f"[bench]  [warmup]", file=sys.stderr)
        t0 = time.monotonic()
        run_once(cmd, cwd)
        print(f"[bench]  [warmup done in {(time.monotonic()-t0)*1000:.1f} ms]", file=sys.stderr)
    rows = []
    for i in range(iters):
        print(f"[bench]  iter {i+1}/{iters}", file=sys.stderr)
        t0 = time.monotonic()
        wall_ms, stdout = run_once(cmd, cwd)
        elapsed = (time.monotonic() - t0) * 1000.0
        compression_ms = parse_timing(stdout)
        print(f"[bench]  iter {i+1} done: wall_ms={wall_ms:.1f}, compression_ms={compression_ms}",
              file=sys.stderr)
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


def parse_process_builds(s: Optional[str], testing_dir: Path) -> list[tuple[str, Path]]:
    """Parse a comma-separated list of label:path entries.  An entry without
    a colon is treated as `process_<label>:<testing_dir>/<label>`.
    When `s` is None the default rpclib + capnp variants are returned,
    resolved under `testing_dir`.
    """
    default_subdirs = [
        ("process_rpclib", "build_rpclib"),
        ("process_capnp", "build_capnp"),
    ]
    if not s:
        return [(label, testing_dir / subdir) for (label, subdir) in default_subdirs]
    out: list[tuple[str, Path]] = []
    for chunk in s.split(","):
        c = chunk.strip()
        if not c:
            continue
        if ":" in c:
            label, path = c.split(":", 1)
            out.append((label.strip(), Path(path.strip()).resolve()))
        else:
            out.append((f"process_{c}", (testing_dir / c).resolve()))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument(
        "--library", choices=[lib.value for lib in Library], default=Library.JPEG.value,
        help="library to benchmark (default: jpeg).  "
             "For zlib, levels are compression levels 1-9 (higher = more compression); "
             "for jpeg, levels are quality values 1-100 (higher = LESS compression).",
    )
    ap.add_argument("--wasm2c-build-dir", type=Path, default=None,
                    help="build dir holding the wasm2c `main` binary "
                         "(default: test/<library>-testing/build)")
    ap.add_argument("--process-builds", type=str, default=None,
                    help="comma-separated process variants to drive, "
                         "either `label:path` or `subdir` (resolved under "
                         "test/<library>-testing/).  Default: rpclib + capnp.")
    ap.add_argument("--sizes", type=str, default="500k, 1m, 4m",
                    help="input sizes, comma-separated; k/m suffixes ok")
    ap.add_argument("--levels", type=str, default=None,
                    help="compression levels (zlib: 1-9, higher = more compression) or "
                         "quality values (jpeg: 1-100, higher = LESS compression). "
                         "Comma-separated. Defaults: zlib -> 1,6,9; jpeg -> 90,50,25,10.")
    ap.add_argument("--iters", type=int, default=3,
                    help="iterations per (backend, size, level)")
    ap.add_argument("--out", type=Path, default=BENCH_DIR / "results.csv")
    ap.add_argument("--batch-size", type=int, default=16,
                    help="number of scanlines per jpeg_write_scanlines call "
                         "(default: 64; only used for jpeg)")
    ap.add_argument("--no-wasm2c", action="store_true",
                    help="skip the wasm2c backend (e.g. if not built)")
    ap.add_argument("--no-process", action="store_true",
                    help="skip the process backends entirely")
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
    levels = parse_ints(args.levels if args.levels is not None else default_levels)
    batch_size = args.batch_size
    # Extra args appended to every binary invocation (batch size for jpeg)
    extra_args = [str(batch_size)] if library is Library.JPEG else []
    source_data = testing_dir / "test_data.txt"
    # -------------------------------------------------------------------------

    main_bin = wasm2c_build_dir / "main"
    if not args.no_wasm2c and not main_bin.exists():
        print(f"[bench] {main_bin} missing; pass --no-wasm2c or rebuild",
              file=sys.stderr)
        return 1
    # Native + wasm2c read test_data.txt from the wasm2c build dir; we still need
    # *some* seed file even when wasm2c is skipped, so fall back to whichever
    # process build we'll use.
    seed_dir = wasm2c_build_dir if not args.no_wasm2c else None

    process_builds: list[tuple[str, Path]] = []
    if not args.no_process:
        process_builds = parse_process_builds(args.process_builds, testing_dir)
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
        if seed_dir is None:
            seed_dir = process_builds[0][1]
    if seed_dir is None:
        print("[bench] nothing to do — all backends disabled", file=sys.stderr)
        return 1

    bench_native = ensure_bench_native(wasm2c_build_dir)

    # Save the original test_data.txt for each build dir we'll be writing to so we
    # can restore them all after benchmarking.
    write_dirs = []
    if not args.no_wasm2c:
        write_dirs.append(wasm2c_build_dir)
    write_dirs.extend(d for _, d in process_builds)
    seed_bytes = source_data.read_bytes()
    backups: list[tuple[Path, Optional[Path]]] = []
    for d in write_dirs:
        test_data = d / "test_data.txt"
        if test_data.exists():
            backup = test_data.with_suffix(".txt.bench_backup")
            shutil.copyfile(test_data, backup)
            backups.append((test_data, backup))
        else:
            backups.append((test_data, None))
    print(f"[bench] staged test_data.txt in {len(write_dirs)} build dir(s)",
          file=sys.stderr)

    all_rows: list[dict] = []
    try:
        for size in sizes:
            print(f"[bench] preparing input for size={size}", file=sys.stderr)
            t0 = time.monotonic()
            for d in write_dirs:
                prepare_input(d, seed_bytes, size, library=library)
            print(f"[bench] input preparation done in {(time.monotonic()-t0)*1000:.1f} ms",
                  file=sys.stderr)
            for level in levels:
                print(f"[bench] size={size} level={level}", file=sys.stderr)

                for label, build_dir in process_builds:
                    rows = run_config(
                        label,
                        [str(build_dir / "main_process"), str(level)] + extra_args,
                        cwd=build_dir,
                        size=size,
                        level=level,
                        iters=args.iters,
                        warmup=True,
                    )
                    all_rows.extend(rows)

                # Native reference — runs from whichever dir we picked for
                # seeding (test_data.txt is identical across them).
                rows = run_config(
                    "native",
                    [str(bench_native), str(level)] + extra_args,
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
                        [str(main_bin), str(level)] + extra_args,
                        cwd=wasm2c_build_dir,
                        size=size,
                        level=level,
                        iters=args.iters,
                        warmup=True,
                    )
                    all_rows.extend(rows)
    finally:
        for test_data, backup in backups:
            if backup is not None:
                shutil.move(str(backup), str(test_data))
            else:
                test_data.unlink(missing_ok=True)
        print("[bench] restored test_data.txt files", file=sys.stderr)

    # Write CSV.
    fields = ["backend", "size_bytes", "level", "iter", "wall_ms", "compression_ms"]
    with open(args.out, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=fields)
        w.writeheader()
        w.writerows(all_rows)
    print(f"[bench] wrote {len(all_rows)} rows -> {args.out}", file=sys.stderr)

    # Short summary: median elapsed time per (backend, size, level).
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

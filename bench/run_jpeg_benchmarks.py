#!/usr/bin/env python3
"""
Drive the libjpeg benchmark across two backends:
    - wasm2c:  test/libjpeg-testing/build/main (RLBox wasm2c sandbox)
    - process: test/libjpeg-testing/build/main_process (process sandbox)

Each binary compresses test/libjpeg-testing/rgb_grid.txt (a fixed 1280x1014
RGB image) at a given quality setting and prints
    COMPRESSION_MS=12.345
We parse that, plus the Python wall-clock wrapper, and record everything to
a CSV.

Unlike zlib, input size isn't varied — rgb_grid.txt is the sole input and
is committed to the repo. Only quality (1..100) is swept.
"""
from __future__ import annotations

import argparse
import csv
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

BENCH_DIR = Path(__file__).resolve().parent
REPO_ROOT = BENCH_DIR.parent
JPEG_TESTING_DIR = REPO_ROOT / "test" / "libjpeg-testing"
DEFAULT_BUILD_DIR = JPEG_TESTING_DIR / "build"

COMPRESSION_RE = re.compile(r"COMPRESSION_MS=([\d.]+)")


def parse_ints(s: str) -> list[int]:
    return [int(x.strip()) for x in s.split(",") if x.strip()]


def run_once(cmd: list[str], cwd: Path) -> tuple[float, str]:
    t0 = time.monotonic()
    res = subprocess.run(
        cmd, cwd=str(cwd), capture_output=True, text=True, check=True
    )
    wall_ms = (time.monotonic() - t0) * 1000.0
    return wall_ms, res.stdout


def parse_timing(stdout: str) -> Optional[float]:
    m = COMPRESSION_RE.search(stdout)
    return float(m.group(1)) if m else None


def run_config(backend: str, cmd: list[str], cwd: Path, quality: int,
               iters: int, warmup: bool, shape: str) -> list[dict]:
    if warmup:
        run_once(cmd, cwd)
    rows = []
    for i in range(iters):
        wall_ms, stdout = run_once(cmd, cwd)
        rows.append(
            dict(
                backend=backend,
                quality=quality,
                shape=shape,
                iter=i,
                wall_ms=wall_ms,
                compression_ms=parse_timing(stdout),
            )
        )
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR,
                    help="build dir holding `main` and `main_process` "
                         "(default: %(default)s)")
    ap.add_argument("--qualities", type=str, default="25,50,75,90",
                    help="JPEG quality settings, comma-separated (1..100)")
    ap.add_argument("--iters", type=int, default=3,
                    help="iterations per (backend, quality)")
    ap.add_argument("--out", type=Path,
                    default=BENCH_DIR / "results" / "jpeg.csv")
    ap.add_argument("--no-wasm2c", action="store_true")
    ap.add_argument("--no-process", action="store_true")
    ap.add_argument("--shape", type=str, default="loop",
                    help="comma-separated list of dispatch shapes: "
                         "`loop` = per-row jpeg_write_scanlines (main / "
                         "main_process), `stress` = one-shot "
                         "jpeg_write_scanlines over the full image "
                         "(main_stress / main_process_stress). Pass "
                         "`loop,stress` to sweep both.")
    args = ap.parse_args()

    build = args.build_dir.resolve()
    qualities = parse_ints(args.qualities)
    shapes = [s.strip() for s in args.shape.split(",") if s.strip()]
    for s in shapes:
        if s not in ("loop", "stress"):
            print(f"[bench] --shape rejects '{s}' (try loop or stress)",
                  file=sys.stderr)
            return 1
    if not shapes:
        print("[bench] --shape cannot be empty", file=sys.stderr)
        return 1
    shape_bins = {
        "loop":   {"wasm2c": "main",        "process": "main_process"},
        "stress": {"wasm2c": "main_stress", "process": "main_process_stress"},
    }

    for sh in shapes:
        if not args.no_wasm2c:
            wbin = build / shape_bins[sh]["wasm2c"]
            if not wbin.exists():
                print(f"[bench] {wbin} missing", file=sys.stderr)
                return 1
        if not args.no_process:
            pbin = build / shape_bins[sh]["process"]
            if not pbin.exists():
                print(f"[bench] {pbin} missing", file=sys.stderr)
                return 1
    if not (build / "rgb_grid.txt").exists():
        print(f"[bench] {build}/rgb_grid.txt missing; copy from "
              f"{JPEG_TESTING_DIR}/rgb_grid.txt", file=sys.stderr)
        return 1
    if not args.no_process and not (build / "sandbox_shim.so").exists():
        print(f"[bench] {build}/sandbox_shim.so missing", file=sys.stderr)
        return 1

    all_rows: list[dict] = []
    for quality in qualities:
        for shape in shapes:
            print(f"[bench] quality={quality} shape={shape}", file=sys.stderr)
            if not args.no_wasm2c:
                wbin = build / shape_bins[shape]["wasm2c"]
                all_rows.extend(run_config(
                    "wasm2c", [str(wbin), str(quality)], build,
                    quality=quality, iters=args.iters, warmup=True,
                    shape=shape))
            if not args.no_process:
                pbin = build / shape_bins[shape]["process"]
                all_rows.extend(run_config(
                    "process", [str(pbin), str(quality)], build,
                    quality=quality, iters=args.iters, warmup=True,
                    shape=shape))

    fields = ["backend", "quality", "shape", "iter", "wall_ms", "compression_ms"]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with open(args.out, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=fields)
        w.writeheader()
        w.writerows(all_rows)
    print(f"[bench] wrote {len(all_rows)} rows -> {args.out}", file=sys.stderr)

    def key(r):
        return (r["backend"], r["quality"], r["shape"])

    by_key: dict = {}
    for r in all_rows:
        by_key.setdefault(key(r), []).append(r)

    print("\nbackend          quality   shape    median_compression_ms",
          file=sys.stderr)
    print("-" * 60, file=sys.stderr)
    for k in sorted(by_key.keys()):
        rows = by_key[k]
        backend, quality, shape = k
        t = statistics.median(r["compression_ms"] for r in rows)
        print(f"{backend:<16} {quality:<9} {shape:<8} {t:>10.2f}",
              file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""
Unified driver for all benchmark suites.

Usage:
    run_benchmarks.py                 # runs every suite
    run_benchmarks.py zlib            # runs only zlib
    run_benchmarks.py zlib jpeg       # runs both explicitly

Each suite is implemented by its own script (run_zlib_benchmarks.py,
run_jpeg_benchmarks.py); this wrapper just dispatches to them and
surfaces a few shared knobs (--iters, --out-dir). For suite-specific
flags (sizes, levels, qualities, meta policies, etc.) invoke the
corresponding run_<suite>_benchmarks.py directly.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

BENCH_DIR = Path(__file__).resolve().parent

SUITES = {
    "zlib": BENCH_DIR / "run_zlib_benchmarks.py",
    "jpeg": BENCH_DIR / "run_jpeg_benchmarks.py",
}


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("suites", nargs="*", choices=list(SUITES),
                    help=f"suites to run (default: all — {', '.join(SUITES)})")
    ap.add_argument("--iters", type=int, default=None,
                    help="iterations per config; forwarded to every suite")
    ap.add_argument("--out-dir", type=Path,
                    default=BENCH_DIR / "results",
                    help="directory to write CSVs into (default: %(default)s)")
    ap.add_argument("--meta-policies", type=str, default=None,
                    help="forwarded to zlib suite (ignored for jpeg)")
    ap.add_argument("--shape", type=str, default=None,
                    help="forwarded to each suite (e.g. `loop`, `stress`, "
                         "or `loop,stress`)")
    args = ap.parse_args()

    picked = args.suites or list(SUITES.keys())
    args.out_dir.mkdir(parents=True, exist_ok=True)

    failures: list[str] = []
    for suite in picked:
        script = SUITES[suite]
        cmd = [sys.executable, str(script),
               "--out", str(args.out_dir / f"{suite}.csv")]
        if args.iters is not None:
            cmd += ["--iters", str(args.iters)]
        if suite == "zlib" and args.meta_policies:
            cmd += ["--meta-policies", args.meta_policies]
        if args.shape:
            cmd += ["--shape", args.shape]
        print(f"\n[run_benchmarks] === {suite} ===", file=sys.stderr)
        print(f"[run_benchmarks] {' '.join(cmd)}", file=sys.stderr)
        rc = subprocess.call(cmd)
        if rc != 0:
            failures.append(f"{suite} (exit {rc})")

    if failures:
        print(f"\n[run_benchmarks] FAILED: {', '.join(failures)}", file=sys.stderr)
        return 1
    print(f"\n[run_benchmarks] all suites OK -> {args.out_dir}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

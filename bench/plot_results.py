#!/usr/bin/env python3
"""
Produce plots from results.csv.

Generates:
    time_vs_size.png    -- median compression time vs input size, per backend,
                           one subplot per compression level (log-log).
    overhead.png        -- overhead factor (sandbox_median / native_median)
                           per backend and size, grouped bars per level.
    throughput.png      -- throughput MB/s per backend and size.
"""
from __future__ import annotations

import argparse
import csv
import statistics
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

BENCH_DIR = Path(__file__).resolve().parent

BACKEND_ORDER = ["native", "wasm2c", "process"]
BACKEND_COLORS = {
    "native": "#1f77b4",
    "wasm2c": "#ff7f0e",
    "process": "#2ca02c",
}
BACKEND_LABELS = {
    "native": "native zlib",
    "wasm2c": "RLBox wasm2c",
    "process": "RLBox process",
}


def load_rows(path: Path) -> list[dict]:
    rows = []
    with open(path, newline="") as fh:
        r = csv.DictReader(fh)
        for row in r:
            rows.append(
                dict(
                    backend=row["backend"],
                    size_bytes=int(row["size_bytes"]),
                    level=int(row["level"]),
                    iter=int(row["iter"]),
                    wall_ms=float(row["wall_ms"]),
                    sandbox_ms=float(row["sandbox_ms"]) if row["sandbox_ms"] else None,
                    native_ms=float(row["native_ms"]) if row["native_ms"] else None,
                )
            )
    return rows


def primary_ms(row: dict) -> float:
    """Time number to chart for this row (sandbox time for sandboxes, native
    time for the native backend)."""
    if row["backend"] == "native":
        return row["native_ms"]
    return row["sandbox_ms"]


def medians(rows: list[dict]) -> dict:
    """Map (backend, size, level) -> median primary_ms."""
    buckets = defaultdict(list)
    for r in rows:
        v = primary_ms(r)
        if v is None:
            continue
        buckets[(r["backend"], r["size_bytes"], r["level"])].append(v)
    return {k: statistics.median(v) for k, v in buckets.items()}


def human_size(n: int) -> str:
    if n >= 1 << 20:
        return f"{n >> 20}M"
    if n >= 1 << 10:
        return f"{n >> 10}K"
    return str(n)


def plot_time_vs_size(rows: list[dict], meds: dict, out_path: Path) -> None:
    backends = [b for b in BACKEND_ORDER if any(r["backend"] == b for r in rows)]
    levels = sorted({r["level"] for r in rows})
    sizes = sorted({r["size_bytes"] for r in rows})

    fig, axes = plt.subplots(
        1, len(levels), figsize=(4.5 * len(levels), 4.2), sharey=True, squeeze=False
    )
    for ax, level in zip(axes[0], levels):
        for backend in backends:
            ys = [meds.get((backend, s, level)) for s in sizes]
            if all(y is None for y in ys):
                continue
            ax.plot(
                sizes,
                ys,
                marker="o",
                label=BACKEND_LABELS[backend],
                color=BACKEND_COLORS[backend],
                linewidth=2,
            )
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_title(f"compression level {level}")
        ax.set_xlabel("input size (bytes, log)")
        ax.grid(True, which="both", linestyle=":", alpha=0.5)
    axes[0][0].set_ylabel("median compression time (ms, log)")
    axes[0][-1].legend(loc="best")
    fig.suptitle("zlib compression time across backends")
    fig.tight_layout()
    fig.savefig(out_path, dpi=130)
    plt.close(fig)
    print(f"[plot] wrote {out_path}")


def plot_overhead(rows: list[dict], meds: dict, out_path: Path) -> None:
    sandbox_backends = [
        b for b in BACKEND_ORDER if b != "native" and any(r["backend"] == b for r in rows)
    ]
    levels = sorted({r["level"] for r in rows})
    sizes = sorted({r["size_bytes"] for r in rows})

    fig, axes = plt.subplots(
        1, len(levels), figsize=(4.5 * len(levels), 4.2), sharey=True, squeeze=False
    )
    x = np.arange(len(sizes))
    width = 0.8 / max(1, len(sandbox_backends))
    for ax, level in zip(axes[0], levels):
        for i, backend in enumerate(sandbox_backends):
            ratios = []
            for s in sizes:
                sbx = meds.get((backend, s, level))
                nat = meds.get(("native", s, level))
                ratios.append(sbx / nat if (sbx and nat) else 0.0)
            offset = (i - (len(sandbox_backends) - 1) / 2) * width
            ax.bar(
                x + offset,
                ratios,
                width,
                label=BACKEND_LABELS[backend],
                color=BACKEND_COLORS[backend],
            )
        ax.axhline(1.0, color="black", linestyle="--", linewidth=1, alpha=0.5)
        ax.set_xticks(x)
        ax.set_xticklabels([human_size(s) for s in sizes])
        ax.set_title(f"compression level {level}")
        ax.set_xlabel("input size")
        ax.grid(True, axis="y", linestyle=":", alpha=0.5)
    axes[0][0].set_ylabel("slowdown vs native (×)")
    axes[0][-1].legend(loc="best")
    fig.suptitle("sandbox overhead relative to native zlib")
    fig.tight_layout()
    fig.savefig(out_path, dpi=130)
    plt.close(fig)
    print(f"[plot] wrote {out_path}")


def plot_throughput(rows: list[dict], meds: dict, out_path: Path) -> None:
    backends = [b for b in BACKEND_ORDER if any(r["backend"] == b for r in rows)]
    levels = sorted({r["level"] for r in rows})
    sizes = sorted({r["size_bytes"] for r in rows})

    fig, axes = plt.subplots(
        1, len(levels), figsize=(4.5 * len(levels), 4.2), sharey=True, squeeze=False
    )
    x = np.arange(len(sizes))
    width = 0.8 / max(1, len(backends))
    for ax, level in zip(axes[0], levels):
        for i, backend in enumerate(backends):
            thr = []
            for s in sizes:
                t = meds.get((backend, s, level))
                # MB/s = (size_bytes / 1e6) / (t_ms / 1000) = size / t * 1e-3
                thr.append((s / (t * 1000)) if t else 0.0)
            offset = (i - (len(backends) - 1) / 2) * width
            ax.bar(
                x + offset,
                thr,
                width,
                label=BACKEND_LABELS[backend],
                color=BACKEND_COLORS[backend],
            )
        ax.set_xticks(x)
        ax.set_xticklabels([human_size(s) for s in sizes])
        ax.set_title(f"compression level {level}")
        ax.set_xlabel("input size")
        ax.grid(True, axis="y", linestyle=":", alpha=0.5)
    axes[0][0].set_ylabel("throughput (MB/s)")
    axes[0][-1].legend(loc="best")
    fig.suptitle("zlib compression throughput across backends")
    fig.tight_layout()
    fig.savefig(out_path, dpi=130)
    plt.close(fig)
    print(f"[plot] wrote {out_path}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--csv", type=Path, default=BENCH_DIR / "results.csv")
    ap.add_argument("--out-dir", type=Path, default=BENCH_DIR / "plots")
    args = ap.parse_args()

    if not args.csv.exists():
        print(f"[plot] {args.csv} not found — run run_benchmarks.py first")
        return 1
    args.out_dir.mkdir(parents=True, exist_ok=True)

    rows = load_rows(args.csv)
    if not rows:
        print("[plot] no rows in csv")
        return 1
    meds = medians(rows)

    plot_time_vs_size(rows, meds, args.out_dir / "time_vs_size.png")
    plot_overhead(rows, meds, args.out_dir / "overhead.png")
    plot_throughput(rows, meds, args.out_dir / "throughput.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

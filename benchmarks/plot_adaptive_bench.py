#!/usr/bin/env python3
"""
Produce overhead plots from results.csv.

Generates:
    overhead_q{level}.png  -- one per quality/compression level: grouped bars,
                              one group per size (plus an "avg" group across
                              sizes), one bar per sandbox backend; y is
                              slowdown vs native.
    overhead_overall.png   -- single chart: x = quality/compression level,
                              grouped bars per sandbox backend, y is overhead
                              averaged across all sizes.
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

# Canonical ordering and styling for known backends.  Any backend name not
# listed here gets the next free matplotlib tab10 color and its raw name as
# the legend label, so adding a new transport (or a new backend entirely) is
# zero-config.
KNOWN_BACKEND_ORDER = [
    "native",
    "wasm2c",
    "process",
    "adaptive",
]
KNOWN_COLORS = {
    "native": "#1f77b4",
    "wasm2c": "#ff7f0e",
    "process": "#2ca02c",
    "adaptive": "#9467bd",
}
KNOWN_LABELS = {
    "native": "native",
    "wasm2c": "wasm2c",
    "process": "process",
    "adaptive": "adaptive",
}
# Fallback palette for unknown backends.
_FALLBACK_COLORS = [
    "#8c564b", "#e377c2", "#7f7f7f", "#bcbd22", "#17becf",
]


def order_backends(rows: list[dict]) -> list[str]:
    """All backends present in the CSV, ordered by the canonical list with
    unknowns appended in first-seen order."""
    seen = []
    for r in rows:
        if r["backend"] not in seen:
            seen.append(r["backend"])
    known = [b for b in KNOWN_BACKEND_ORDER if b in seen]
    extra = [b for b in seen if b not in known]
    return known + extra


def color_for(backend: str, idx: int) -> str:
    return KNOWN_COLORS.get(backend, _FALLBACK_COLORS[idx % len(_FALLBACK_COLORS)])


def label_for(backend: str) -> str:
    return KNOWN_LABELS.get(backend, backend)


def load_rows(path: Path) -> list[dict]:
    rows = []
    with open(path, newline="") as fh:
        r = csv.DictReader(fh)
        for row in r:
            rows.append(
                dict(
                    backend=row["backend"],
                    level=int(row["level"]),
                    size=int(row["size"]),
                    iter=int(row["iter"]),
                    inner_iter=int(row["inner_iter"]),
                    compression_ms=float(row["compression_ms"]),
                )
            )
    return rows


def medians(rows: list[dict]) -> dict:
    """Map (backend, level, size) -> median compression_ms."""
    buckets = defaultdict(list)
    for r in rows:
        buckets[(r["backend"], r["level"], r["size"])].append(r["compression_ms"])
    return {k: statistics.median(v) for k, v in buckets.items()}


def human_size(n: int) -> str:
    if n >= 1 << 20:
        return f"{n >> 20}M"
    if n >= 1 << 10:
        return f"{n >> 10}K"
    return str(n)


def plot_overhead_per_level(
    rows: list[dict], meds: dict, level: int, sizes: list[int], out_path: Path
) -> None:
    """One chart for a single quality/compression level.

    X groups: each size, plus a final "avg" group averaging the per-size
    overheads.  Bars within a group: one per sandbox backend.
    """
    sandbox_backends = [b for b in order_backends(rows) if b != "native"]
    if not sandbox_backends:
        return

    group_labels = [f"{human_size(s).lower()}-phase" for s in sizes] + ["Overall"]
    x = np.arange(len(group_labels))
    width = 0.8 / max(1, len(sandbox_backends))

    fig, ax = plt.subplots(figsize=(6, 4.2))
    for i, backend in enumerate(sandbox_backends):
        per_size_ratios = []
        for s in sizes:
            sbx = meds.get((backend, level, s))
            nat = meds.get(("native", level, s))
            per_size_ratios.append(sbx / nat if (sbx and nat) else 0.0)
        nonzero = [r for r in per_size_ratios if r > 0]
        avg_ratio = statistics.mean(nonzero) if nonzero else 0.0
        ratios = per_size_ratios + [avg_ratio]
        offset = (i - (len(sandbox_backends) - 1) / 2) * width
        ax.bar(
            x + offset,
            ratios,
            width,
            label=label_for(backend),
            color=color_for(backend, i),
        )
    ax.axhline(1.0, color="black", linestyle="--", linewidth=1, alpha=0.5)
    ax.set_xticks(x)
    ax.set_xticklabels(group_labels)
    ax.set_xlabel("input size (bytes)")
    ax.set_ylabel("slowdown vs native (x)")
    ax.grid(True, axis="y", linestyle=":", alpha=0.5)
    ax.legend(loc="best")
    fig.suptitle(f"sandbox overhead vs native - level {level}")
    fig.tight_layout()
    fig.savefig(out_path, dpi=130)
    plt.close(fig)
    print(f"[plot] wrote {out_path}")


def plot_overhead_overall(
    rows: list[dict], meds: dict, levels: list[int], sizes: list[int], out_path: Path
) -> None:
    """Single chart: x = levels, bars per sandbox backend, y = overhead
    averaged across all sizes."""
    sandbox_backends = [b for b in order_backends(rows) if b != "native"]
    if not sandbox_backends:
        return

    x = np.arange(len(levels))
    width = 0.8 / max(1, len(sandbox_backends))

    fig, ax = plt.subplots(figsize=(6, 4.2))
    for i, backend in enumerate(sandbox_backends):
        ratios = []
        for lvl in levels:
            per_size = []
            for s in sizes:
                sbx = meds.get((backend, lvl, s))
                nat = meds.get(("native", lvl, s))
                if sbx and nat:
                    per_size.append(sbx / nat)
            ratios.append(statistics.mean(per_size) if per_size else 0.0)
        offset = (i - (len(sandbox_backends) - 1) / 2) * width
        ax.bar(
            x + offset,
            ratios,
            width,
            label=label_for(backend),
            color=color_for(backend, i),
        )
    ax.axhline(1.0, color="black", linestyle="--", linewidth=1, alpha=0.5)
    ax.set_xticks(x)
    ax.set_xticklabels([str(lvl) for lvl in levels])
    ax.set_xlabel("quality / compression level")
    ax.set_ylabel("slowdown vs native (x, avg over sizes)")
    ax.grid(True, axis="y", linestyle=":", alpha=0.5)
    ax.legend(loc="best")
    fig.suptitle("sandbox overhead vs native - averaged across sizes")
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
        print(f"[plot] {args.csv} not found - run run_adaptive_bench.py first")
        return 1
    args.out_dir.mkdir(parents=True, exist_ok=True)

    rows = load_rows(args.csv)
    if not rows:
        print("[plot] no rows in csv")
        return 1
    meds = medians(rows)

    levels = sorted({r["level"] for r in rows})
    sizes = sorted({r["size"] for r in rows})

    for lvl in levels:
        plot_overhead_per_level(
            rows, meds, lvl, sizes, args.out_dir / f"overhead_q{lvl}.png"
        )
    plot_overhead_overall(
        rows, meds, levels, sizes, args.out_dir / "overhead_overall.png"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
#!/usr/bin/env python3
"""
Produce overhead plot from run_process_bench.py's results.csv.

Generates:
    overhead.png  -- a single figure with one subplot per quality/compression
                     level. Each subplot has grouped bars: one group per size,
                     one bar per sandbox backend (wasm2c, process_rpclib,
                     process_capnp, ...); y is slowdown vs native.
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

KNOWN_BACKEND_ORDER = [
    "native",
    "wasm2c",
    "process_rpclib",
    "process_capnp",
]
KNOWN_COLORS = {
    "native": "#1f77b4",
    "wasm2c": "#ff7f0e",
    "process_rpclib": "#2ca02c",
    "process_capnp": "#d62728",
}
KNOWN_LABELS = {
    "native": "native",
    "wasm2c": "wasm2c",
    "process_rpclib": "process (rpclib)",
    "process_capnp": "process (capnp)",
}
_FALLBACK_COLORS = [
    "#9467bd", "#8c564b", "#e377c2", "#7f7f7f", "#bcbd22", "#17becf",
]


def order_backends(rows: list[dict]) -> list[str]:
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


def plot_overhead_combined(
    rows: list[dict], meds: dict, levels: list[int], sizes: list[int], out_path: Path
) -> None:
    """Single figure with one subplot per level.

    Each subplot: x groups are sizes, bars within a group are sandbox backends,
    y is slowdown vs native at that (level, size).
    """
    sandbox_backends = [b for b in order_backends(rows) if b != "native"]
    if not sandbox_backends or not levels:
        return

    group_labels = [human_size(s) for s in sizes]
    x = np.arange(len(group_labels))
    width = 0.8 / max(1, len(sandbox_backends))

    n = len(levels)
    ncols = min(n, 2)
    nrows = (n + ncols - 1) // ncols
    fig, axes = plt.subplots(
        nrows, ncols, figsize=(6 * ncols, 4.2 * nrows), squeeze=False
    )

    for idx, level in enumerate(levels):
        ax = axes[idx // ncols][idx % ncols]
        for i, backend in enumerate(sandbox_backends):
            ratios = []
            for s in sizes:
                sbx = meds.get((backend, level, s))
                nat = meds.get(("native", level, s))
                ratios.append(sbx / nat if (sbx and nat) else 0.0)
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
        ax.set_title(f"level {level}")
        ax.grid(True, axis="y", linestyle=":", alpha=0.5)

    # Hide any unused axes.
    for j in range(n, nrows * ncols):
        axes[j // ncols][j % ncols].axis("off")

    # Single legend for the whole figure.
    handles, labels = axes[0][0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", ncol=len(labels))
    fig.suptitle("sandbox overhead vs native")
    fig.tight_layout(rect=[0, 0.05, 1, 0.97])
    fig.savefig(out_path, dpi=130)
    plt.close(fig)
    print(f"[plot] wrote {out_path}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--csv", type=Path, default=BENCH_DIR / "results.csv")
    ap.add_argument("--out-dir", type=Path, default=BENCH_DIR / "plots")
    args = ap.parse_args()

    if not args.csv.exists():
        print(f"[plot] {args.csv} not found - run run_process_bench.py first")
        return 1
    args.out_dir.mkdir(parents=True, exist_ok=True)

    rows = load_rows(args.csv)
    if not rows:
        print("[plot] no rows in csv")
        return 1
    meds = medians(rows)

    levels = sorted({r["level"] for r in rows})
    sizes = sorted({r["size"] for r in rows})

    plot_overhead_combined(rows, meds, levels, sizes, args.out_dir / "overhead.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

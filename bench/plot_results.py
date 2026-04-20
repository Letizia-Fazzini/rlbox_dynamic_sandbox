#!/usr/bin/env python3
"""
Produce plots from a benchmark CSV.

Shapes auto-detected from the header:
    zlib: backend, size_bytes, level, iter, wall_ms, (compression_ms | sandbox_ms, native_ms)
        -> one PNG with rows (time, overhead, throughput) x cols (levels)
    jpeg: backend, quality, iter, wall_ms, compression_ms
        -> one PNG with subplots (time vs quality, throughput vs quality)

Per-backend color is stable across every subplot.  Meta ablations
(meta_wasm, meta_process) are drawn in the same hue as their pure
counterpart but with dashed lines / hatched bars / reduced alpha, so
they read as ablations of wasm2c / process rather than independent
points.  meta_adaptive gets a distinct solid color of its own.
"""
from __future__ import annotations

import argparse
import csv
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Optional

import matplotlib.pyplot as plt
import numpy as np
from matplotlib.lines import Line2D
from matplotlib.patches import Patch

BENCH_DIR = Path(__file__).resolve().parent

# Stable per-backend visual identity.  "pair" groups a meta ablation with
# its pure counterpart so both share a hue; meta_adaptive stands alone.
BACKEND_STYLE = {
    "native":         dict(color="#1f77b4", label="native",          kind="pure"),
    "wasm2c":         dict(color="#ff7f0e", label="RLBox wasm2c",    kind="pure"),
    "process":        dict(color="#2ca02c", label="RLBox process",   kind="pure"),
    "process_capnp":  dict(color="#2ca02c", label="process (capnp)",  kind="pure"),
    "process_rpclib": dict(color="#bcbd22", label="process (rpclib)", kind="transport_alt"),
    "meta_wasm":      dict(color="#ff7f0e", label="meta (pin wasm)", kind="ablation"),
    "meta_process":   dict(color="#2ca02c", label="meta (pin proc)", kind="ablation"),
    "meta_adaptive":  dict(color="#9467bd", label="meta adaptive",   kind="star"),
}
_FALLBACK_COLORS = ["#8c564b", "#e377c2", "#7f7f7f", "#bcbd22", "#17becf"]

# Canonical ordering when multiple backends coexist.
BACKEND_ORDER = [
    "native", "wasm2c", "process", "process_capnp", "process_rpclib",
    "meta_wasm", "meta_process", "meta_adaptive",
]


def style_for(backend: str, fallback_idx: int) -> dict:
    if backend in BACKEND_STYLE:
        return BACKEND_STYLE[backend]
    return dict(
        color=_FALLBACK_COLORS[fallback_idx % len(_FALLBACK_COLORS)],
        label=backend,
        kind="pure",
    )


def line_kwargs(style: dict) -> dict:
    if style["kind"] == "ablation":
        return dict(color=style["color"], linestyle="--", marker="s",
                    alpha=0.65, linewidth=1.8)
    if style["kind"] == "star":
        return dict(color=style["color"], linestyle="-", marker="D",
                    linewidth=2.6)
    if style["kind"] == "transport_alt":
        return dict(color=style["color"], linestyle="-", marker="^",
                    linewidth=2.0)
    return dict(color=style["color"], linestyle="-", marker="o", linewidth=2.0)


def bar_kwargs(style: dict) -> dict:
    if style["kind"] == "ablation":
        return dict(color=style["color"], alpha=0.55, hatch="//",
                    edgecolor="white", linewidth=0.6)
    if style["kind"] == "star":
        return dict(color=style["color"], edgecolor="black", linewidth=1.0)
    return dict(color=style["color"])


def order_backends(backends: set[str]) -> list[str]:
    known = [b for b in BACKEND_ORDER if b in backends]
    extras = sorted(b for b in backends if b not in set(BACKEND_ORDER))
    return known + extras


def detect_shape(fieldnames: list[str]) -> str:
    f = set(fieldnames)
    if "quality" in f:
        return "jpeg"
    if "size_bytes" in f and "level" in f:
        return "zlib"
    raise SystemExit(f"[plot] unrecognized CSV schema: {fieldnames}")


def load_rows(path: Path) -> tuple[list[dict], str]:
    with open(path, newline="") as fh:
        r = csv.DictReader(fh)
        shape = detect_shape(r.fieldnames or [])
        rows = []
        for row in r:
            # Tolerate both schemas: new `compression_ms` or old
            # `sandbox_ms`/`native_ms`.  In the old schema, `native_ms`
            # was populated only on native rows and `sandbox_ms` only on
            # sandboxed rows; collapse both into a single timing field.
            t = row.get("compression_ms")
            if not t:
                t = row.get("sandbox_ms") or row.get("native_ms")
            t = float(t) if t else None
            rec = dict(
                backend=row["backend"],
                iter=int(row["iter"]),
                wall_ms=float(row["wall_ms"]),
                timing_ms=t,
                # `dispatch_shape` (optional): loop = per-chunk/per-row
                # invokes, stress = one-shot bulk invoke.  Older CSVs
                # don't have this column; default to "loop".
                dispatch_shape=row.get("shape") or "loop",
            )
            if shape == "zlib":
                rec["size_bytes"] = int(row["size_bytes"])
                rec["level"] = int(row["level"])
            else:
                rec["quality"] = int(row["quality"])
            rows.append(rec)
    return rows, shape


def zlib_medians(rows: list[dict]) -> dict:
    buckets = defaultdict(list)
    for r in rows:
        if r["timing_ms"] is None:
            continue
        buckets[(r["backend"], r["size_bytes"], r["level"])].append(r["timing_ms"])
    return {k: statistics.median(v) for k, v in buckets.items()}


def jpeg_medians(rows: list[dict]) -> dict:
    buckets = defaultdict(list)
    for r in rows:
        if r["timing_ms"] is None:
            continue
        buckets[(r["backend"], r["quality"])].append(r["timing_ms"])
    return {k: statistics.median(v) for k, v in buckets.items()}


def human_size(n: int) -> str:
    if n >= 1 << 20:
        return f"{n >> 20}M"
    if n >= 1 << 10:
        return f"{n >> 10}K"
    return str(n)


def legend_handles(backends: list[str]) -> list:
    handles: list = []
    for i, b in enumerate(backends):
        s = style_for(b, i)
        if s["kind"] == "ablation":
            handles.append(Patch(facecolor=s["color"], alpha=0.55, hatch="//",
                                 edgecolor="white", label=s["label"]))
        else:
            marker = {"star": "D", "transport_alt": "^"}.get(s["kind"], "o")
            handles.append(Line2D([0], [0], color=s["color"],
                                  linewidth=2.4 if s["kind"] == "star" else 2.0,
                                  marker=marker, label=s["label"]))
    return handles


def zlib_title(backends: list[str], dispatch_shape: Optional[str] = None) -> str:
    bset = set(backends)
    if {"process_capnp", "process_rpclib"} <= bset \
            and not (bset & {"wasm2c", "meta_wasm", "meta_process", "meta_adaptive"}):
        base = "zlib: process-sandbox transport ablation (capnp vs rpclib)"
    else:
        base = "zlib: sandbox backends across input size & level"
    if dispatch_shape == "loop":
        return f"{base}  [loop shape: per-chunk deflate]"
    if dispatch_shape == "stress":
        return f"{base}  [stress shape: one-shot deflate(Z_FINISH)]"
    return base


def plot_zlib(rows: list[dict], out_path: Path,
              dispatch_shape: Optional[str] = None) -> None:
    meds = zlib_medians(rows)
    backends = order_backends({r["backend"] for r in rows})
    sandbox_backends = [b for b in backends if b != "native"]
    levels = sorted({r["level"] for r in rows})
    sizes = sorted({r["size_bytes"] for r in rows})
    x = np.arange(len(sizes))

    fig, axes = plt.subplots(
        3, len(levels),
        figsize=(4.6 * len(levels), 11.5),
        squeeze=False,
    )

    # --- row 0: time vs size (log-log) ---
    for col, level in enumerate(levels):
        ax = axes[0][col]
        for i, backend in enumerate(backends):
            ys = [meds.get((backend, s, level)) for s in sizes]
            if all(y is None for y in ys):
                continue
            ax.plot(sizes, ys, **line_kwargs(style_for(backend, i)))
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_title(f"compression level {level}")
        ax.set_xlabel("input size (bytes, log)")
        ax.grid(True, which="both", linestyle=":", alpha=0.5)
    axes[0][0].set_ylabel("median compression time (ms, log)")

    # --- row 1: overhead relative to native (grouped bars) ---
    width = 0.8 / max(1, len(sandbox_backends))
    for col, level in enumerate(levels):
        ax = axes[1][col]
        for i, backend in enumerate(sandbox_backends):
            ratios = []
            for s in sizes:
                sbx = meds.get((backend, s, level))
                nat = meds.get(("native", s, level))
                ratios.append(sbx / nat if (sbx and nat) else 0.0)
            offset = (i - (len(sandbox_backends) - 1) / 2) * width
            ax.bar(x + offset, ratios, width,
                   **bar_kwargs(style_for(backend, i)))
        ax.axhline(1.0, color="black", linestyle="--", linewidth=1, alpha=0.5)
        ax.set_xticks(x)
        ax.set_xticklabels([human_size(s) for s in sizes])
        ax.set_xlabel("input size")
        ax.grid(True, axis="y", linestyle=":", alpha=0.5)
    axes[1][0].set_ylabel("slowdown vs native (x)")

    # --- row 2: throughput (grouped bars, MB/s) ---
    width = 0.8 / max(1, len(backends))
    for col, level in enumerate(levels):
        ax = axes[2][col]
        for i, backend in enumerate(backends):
            thr = []
            for s in sizes:
                t = meds.get((backend, s, level))
                # MB/s = (size / 1e6) / (t / 1000) = size / (t * 1000)
                thr.append((s / (t * 1000)) if t else 0.0)
            offset = (i - (len(backends) - 1) / 2) * width
            ax.bar(x + offset, thr, width,
                   **bar_kwargs(style_for(backend, i)))
        ax.set_xticks(x)
        ax.set_xticklabels([human_size(s) for s in sizes])
        ax.set_xlabel("input size")
        ax.grid(True, axis="y", linestyle=":", alpha=0.5)
    axes[2][0].set_ylabel("throughput (MB/s)")

    fig.legend(handles=legend_handles(backends),
               loc="upper center", ncol=min(len(backends), 5),
               bbox_to_anchor=(0.5, 0.98), frameon=False)
    fig.suptitle(zlib_title(backends, dispatch_shape), y=1.005, fontsize=13)
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    fig.savefig(out_path, dpi=130, bbox_inches="tight")
    plt.close(fig)
    print(f"[plot] wrote {out_path}")


def plot_jpeg(rows: list[dict], out_path: Path,
              dispatch_shape: Optional[str] = None) -> None:
    meds = jpeg_medians(rows)
    backends = order_backends({r["backend"] for r in rows})
    qualities = sorted({r["quality"] for r in rows})
    x = np.arange(len(qualities))

    # rgb_grid.txt is 1280x1014 RGB = 3,893,760 bytes of raw input.
    # Use that to translate compression time into MB/s throughput.
    raw_bytes = 1280 * 1014 * 3

    fig, axes = plt.subplots(1, 2, figsize=(11, 4.4), squeeze=False)

    ax = axes[0][0]
    width = 0.8 / max(1, len(backends))
    for i, backend in enumerate(backends):
        ys = [meds.get((backend, q), 0.0) for q in qualities]
        offset = (i - (len(backends) - 1) / 2) * width
        ax.bar(x + offset, ys, width, **bar_kwargs(style_for(backend, i)))
    ax.set_xticks(x)
    ax.set_xticklabels([str(q) for q in qualities])
    ax.set_xlabel("JPEG quality")
    ax.set_ylabel("median compression time (ms)")
    ax.set_title("compression time")
    ax.grid(True, axis="y", linestyle=":", alpha=0.5)

    ax = axes[0][1]
    for i, backend in enumerate(backends):
        thr = []
        for q in qualities:
            t = meds.get((backend, q))
            thr.append((raw_bytes / (t * 1000)) if t else 0.0)
        offset = (i - (len(backends) - 1) / 2) * width
        ax.bar(x + offset, thr, width, **bar_kwargs(style_for(backend, i)))
    ax.set_xticks(x)
    ax.set_xticklabels([str(q) for q in qualities])
    ax.set_xlabel("JPEG quality")
    ax.set_ylabel("throughput (MB/s of raw input)")
    ax.set_title("throughput")
    ax.grid(True, axis="y", linestyle=":", alpha=0.5)

    fig.legend(handles=legend_handles(backends),
               loc="upper center", ncol=len(backends),
               bbox_to_anchor=(0.5, 1.02), frameon=False)
    title = "libjpeg-turbo: sandbox backends across quality"
    if dispatch_shape == "loop":
        title += "  [loop shape: per-row jpeg_write_scanlines]"
    elif dispatch_shape == "stress":
        title += "  [stress shape: one-shot bulk jpeg_write_scanlines]"
    fig.suptitle(title, y=1.08, fontsize=13)
    fig.tight_layout()
    fig.savefig(out_path, dpi=130, bbox_inches="tight")
    plt.close(fig)
    print(f"[plot] wrote {out_path}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--csv", type=Path,
                    default=BENCH_DIR / "results" / "zlib.csv",
                    help="input CSV (default: %(default)s)")
    ap.add_argument("--out", type=Path, default=None,
                    help="output PNG path (default: plots/<stem>.png)")
    ap.add_argument("--out-dir", type=Path, default=BENCH_DIR / "plots",
                    help="output directory when --out unspecified "
                         "(default: %(default)s)")
    args = ap.parse_args()

    if not args.csv.exists():
        print(f"[plot] {args.csv} not found — run run_benchmarks.py first, "
              f"or pass --csv <path>")
        return 1

    rows, shape = load_rows(args.csv)
    if not rows:
        print("[plot] no rows in csv")
        return 1

    # Split by dispatch_shape.  If only one shape is present, produce a
    # single plot at the requested path (preserves old behavior).  If
    # both loop + stress are present, emit one file per shape with a
    # `_<dispatch_shape>` suffix so they're comparable side by side.
    shapes_seen = sorted({r["dispatch_shape"] for r in rows})
    plot_fn = plot_zlib if shape == "zlib" else plot_jpeg

    if args.out is not None:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        base_out = args.out
        out_dir = args.out.parent
        stem = args.out.stem
        ext = args.out.suffix or ".png"
    else:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        base_out = args.out_dir / f"{args.csv.stem}.png"
        out_dir = args.out_dir
        stem = args.csv.stem
        ext = ".png"

    if len(shapes_seen) <= 1:
        ds = shapes_seen[0] if shapes_seen else None
        plot_fn(rows, base_out, dispatch_shape=ds)
    else:
        for ds in shapes_seen:
            subset = [r for r in rows if r["dispatch_shape"] == ds]
            out = out_dir / f"{stem}_{ds}{ext}"
            plot_fn(subset, out, dispatch_shape=ds)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

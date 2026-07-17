#!/usr/bin/env python3
"""The only Python in this repo: turns results/results.csv into PNG charts and
a markdown summary. Requires matplotlib (see README for the venv setup)."""
import csv
import statistics
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt

RESULTS_DIR = Path(__file__).parent / "results"
CSV_PATH = RESULTS_DIR / "results.csv"
POLICY_ORDER = ["FIFO", "LRU", "CLOCK", "SIEVE", "S3-FIFO"]
COLORS = {
    "FIFO": "#888888",
    "LRU": "#1f77b4",
    "CLOCK": "#2ca02c",
    "SIEVE": "#d62728",
    "S3-FIFO": "#9467bd",
}


def load_rows():
    with open(CSV_PATH, newline="") as f:
        return list(csv.DictReader(f))


def plot_workload(workload, rows):
    fig, ax = plt.subplots(figsize=(6, 4))
    by_policy = defaultdict(list)
    for r in rows:
        by_policy[r["policy"]].append((int(r["cache_size"]), float(r["miss_ratio"])))
    for policy in POLICY_ORDER:
        pts = sorted(by_policy.get(policy, []))
        if not pts:
            continue
        xs, ys = zip(*pts)
        ax.plot(xs, ys, marker="o", label=policy, color=COLORS.get(policy))
    ax.set_xlabel("cache size (frames)")
    ax.set_ylabel("miss ratio")
    ax.set_title(f"miss ratio vs cache size -- {workload}")
    ax.legend()
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    out = RESULTS_DIR / f"{workload}_miss_ratio.png"
    fig.savefig(out, dpi=120)
    plt.close(fig)
    return out


def build_summary(rows):
    workloads = sorted(set(r["workload"] for r in rows))
    lines = ["# Benchmark summary\n"]

    for workload in workloads:
        wrows = [r for r in rows if r["workload"] == workload]
        cache_sizes = sorted(set(int(r["cache_size"]) for r in wrows))
        lines.append(f"\n## {workload} -- miss ratio by cache size\n")
        lines.append("| cache_size | " + " | ".join(POLICY_ORDER) + " |")
        lines.append("|---" * (len(POLICY_ORDER) + 1) + "|")
        by_key = {(r["policy"], int(r["cache_size"])): float(r["miss_ratio"]) for r in wrows}
        for cs in cache_sizes:
            cells = [f"{by_key.get((p, cs), float('nan')):.4f}" for p in POLICY_ORDER]
            lines.append(f"| {cs} | " + " | ".join(cells) + " |")

    lines.append("\n## ops/sec (averaged across all workloads and cache sizes)\n")
    lines.append("| policy | avg ops/sec |")
    lines.append("|---|---|")
    by_policy_ops = defaultdict(list)
    for r in rows:
        by_policy_ops[r["policy"]].append(float(r["ops_per_sec"]))
    for p in POLICY_ORDER:
        vals = by_policy_ops.get(p, [])
        avg = statistics.mean(vals) if vals else float("nan")
        lines.append(f"| {p} | {avg:,.0f} |")

    return "\n".join(lines) + "\n"


def main():
    rows = load_rows()
    workloads = sorted(set(r["workload"] for r in rows))

    png_paths = [plot_workload(w, [r for r in rows if r["workload"] == w]) for w in workloads]

    summary = build_summary(rows)
    summary_path = RESULTS_DIR / "summary.md"
    summary_path.write_text(summary, encoding="utf-8")

    print(summary)
    print(f"wrote {len(png_paths)} PNGs and {summary_path}")


if __name__ == "__main__":
    main()

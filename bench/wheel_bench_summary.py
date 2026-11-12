"""Summarize wheel_bench.sh logs by docs/design/wheel-evidence.md's rule, as A1 applies it.

Per row, size, pad and arm: the median over rounds of each metric with its min-max, and the core
clock (ghz0). Arms are compared round by round: each round runs every arm once, so a candidate's
ratio is the median over rounds of candidate / reference in that round, with a 95% interval from
order statistics (distribution-free). The A/A band is the farther end of the twin / reference
interval from 1.00: how far an arm identical to the reference lands. The min-max band (the pair's
larger relative min-max spread, or their medians' difference if wider) is printed beside it.

A candidate passes when its interval's top is at most 1.10, fails when its bottom is above, and is
inconclusive otherwise. With --gate ARM, exits 1 when ARM's median paired ratio exceeds 1.10 in any
row. --ref and --twin replace the reference and twin arms; --arms limits the arms compared.
"""

from __future__ import annotations

import argparse
import math
import statistics
import sys
from collections import defaultdict

THRESHOLD = 1.10
METRICS: dict[str, list[str]] = {
    "single": ["arm_ns", "rearm_ns", "cancel_ns", "expire_ns", "score"],
    "cross": [
        "producer_ns",
        "producer_cpu_ns",
        "span_ms",
        "max_late_ms",
        "last_after_deadline_ms",
    ],
}
PAIRED: dict[str, list[str]] = {
    "single": ["score", "arm_ns", "rearm_ns", "cancel_ns", "expire_ns"],
    "cross": ["producer_ns", "producer_cpu_ns"],
}
SIZE = {"single": "n", "cross": "p"}
REF = {"single": ("a", "a2"), "cross": ("f", "f2")}

Key = tuple[str, int, int]
Run = dict[str, str]
Arms = dict[str, dict[int, Run]]


def parse(paths: list[str]) -> dict[Key, Arms]:
    """Runs by row key, arm and round (the last `round=` line before each)."""
    runs: dict[Key, Arms] = defaultdict(lambda: defaultdict(dict))
    for path in paths:
        current = -1
        with open(path) as f:
            for line in f:
                if line.startswith("round="):
                    current = int(line.split()[0].split("=", 1)[1])
                if not line.startswith("row="):
                    continue
                fields = dict(kv.split("=", 1) for kv in line.split())
                row = fields["row"]
                key = (row, int(fields[SIZE[row]]), int(fields.get("pad", "0")))
                if current in runs[key][fields["arm"]]:
                    sys.exit(
                        f"{path}: {fields['arm']} ran twice in round {current} of {key}"
                    )
                runs[key][fields["arm"]][current] = fields
    return runs


def median_interval(values: list[float]) -> tuple[float, float, float]:
    """The median and a distribution-free interval of at least 95% (order statistics)."""
    xs = sorted(values)
    n = len(xs)
    k = 0  # the largest k with P(Binomial(n, 1/2) < k) <= 0.025
    cumulative = 0.0
    while k < n and cumulative + math.comb(n, k) / 2**n <= 0.025:
        cumulative += math.comb(n, k) / 2**n
        k += 1
    if k == 0:
        return statistics.median(xs), xs[0], xs[-1]
    return statistics.median(xs), xs[k - 1], xs[n - k]


def paired(arms: Arms, num: str, den: str, metric: str) -> list[float]:
    rounds = sorted(set(arms[num]) & set(arms[den]))
    return [float(arms[num][r][metric]) / float(arms[den][r][metric]) for r in rounds]


def column(arms: Arms, arm: str, metric: str) -> list[float]:
    return [float(r[metric]) for r in arms[arm].values()]


def spread(values: list[float]) -> float:
    m = statistics.median(values)
    return (max(values) - min(values)) / m if m else 0.0


def fmt(m: float, lo: float, hi: float) -> str:
    return f"{m:.3f} [{lo:.3f}-{hi:.3f}]"


def summarize(key: Key, arms: Arms, ref: str, twin: str, gate: str | None) -> bool:
    """Prints one row; returns whether `gate` missed 1.10 there."""
    row, size, pad = key
    decide = PAIRED[row][0]
    rounds = len({r for rs in arms.values() for r in rs})
    print(
        f"\n{row} {SIZE[row]}={size}"
        + (f" pad={pad}" if pad else "")
        + f" rounds={rounds}"
    )
    for arm in sorted(arms):
        rs = list(arms[arm].values())
        cells = []
        for metric in METRICS[row] + ["ghz0"]:
            if metric in rs[0]:
                xs = column(arms, arm, metric)
                cells.append(
                    f"{metric}={statistics.median(xs):.2f} [{min(xs):.2f}-{max(xs):.2f}]"
                )
        if row == "cross":
            cells.append(f"valid={all(r['valid'] == '1' for r in rs)}")
        print(f"  {arm:6} runs={len(rs)} " + " ".join(cells))
    if ref not in arms:
        return False
    metrics = [m for m in PAIRED[row] if m in next(iter(arms[ref].values()))]
    if twin in arms:
        cells = []
        band = 0.0
        for metric in metrics:
            m, lo, hi = median_interval(paired(arms, twin, ref, metric))
            cells.append(f"{metric} {fmt(m, lo, hi)}")
            if metric == decide:
                band = max(abs(lo - 1), abs(hi - 1))
        xs, ys = column(arms, ref, decide), column(arms, twin, decide)
        mx, my = statistics.median(xs), statistics.median(ys)
        minmax = max(abs(mx - my) / mx, spread(xs), spread(ys))
        print(f"  A/A band on {decide}: {band:.1%} (min-max band {minmax:.1%})")
        print(f"  {twin}/{ref} " + ", ".join(cells))
    missed = False
    for arm in sorted(arms):
        if arm in (ref, twin):
            continue
        cells = []
        verdict = ""
        for metric in metrics:
            ratios = paired(arms, arm, ref, metric)
            if not ratios:
                continue
            m, lo, hi = median_interval(ratios)
            cells.append(f"{metric} {fmt(m, lo, hi)}")
            if metric == decide:
                of_medians = statistics.median(
                    column(arms, arm, metric)
                ) / statistics.median(column(arms, ref, metric))
                verdict = (
                    "pass"
                    if hi <= THRESHOLD
                    else "fail"
                    if lo > THRESHOLD
                    else "inconclusive"
                )
                verdict += f" (ratio of medians {of_medians:.3f})"
                missed |= arm == gate and m > THRESHOLD
        print(
            f"  {arm}/{ref} " + ", ".join(cells) + (f": {verdict}" if verdict else "")
        )
    return missed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--gate")
    parser.add_argument("--ref")
    parser.add_argument("--twin")
    parser.add_argument(
        "--arms", help="comma-separated arms to compare (default: every arm)"
    )
    parser.add_argument("logs", nargs="+")
    args = parser.parse_args()
    failed: list[str] = []
    for key, arms in sorted(parse(args.logs).items()):
        if args.arms:
            keep = set(args.arms.split(","))
            arms = {a: rs for a, rs in arms.items() if a in keep}
        row = key[0]
        ref, twin = args.ref or REF[row][0], args.twin or REF[row][1]
        if summarize(key, arms, ref, twin, args.gate):
            failed.append(f"{row} {SIZE[row]}={key[1]}")
    if args.gate is not None:
        print(
            f"\ngate {args.gate}: "
            + ("fail at " + ", ".join(failed) if failed else "every row within 1.10x")
        )
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

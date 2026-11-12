"""wheel_bench_summary.py --gate on synthetic logs: it passes what is within 1.10x and exits
non-zero for a real miss, for a row that compares nothing, and for a metric that reads 0."""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

SUMMARY = Path(__file__).resolve().parent.parent / "bench" / "wheel_bench_summary.py"
ROUNDS = range(6)


def single(r: int, arm: str, score: float) -> str:
    return (
        f"round={r}\nrow=single arm={arm} n=1000 pad=0 reps=1 arm_ns=1 rearm_ns=1 cancel_ns=1 "
        f"expire_ns=1 score={score} fired=1 ghz0=3 ghz1=3\n"
    )


def cross(r: int, arm: str, ns: float, cpu: float) -> str:
    return (
        f"round={r}\nrow=cross arm={arm} p=1 producer_ns={ns} producer_cpu_ns={cpu} "
        "producer_min_ns=1 producer_max_ns=1 span_ms=1 last_after_deadline_ms=1 max_late_ms=1 "
        "fired=1 early=0 twice=0 missing=0 valid=1 ghz0=3 ghz1=3\n"
    )


def log(*parts: str) -> str:
    return "".join(parts)


def gate(name: str, text: str, arm: str = "w") -> tuple[int, str]:
    with tempfile.TemporaryDirectory() as d:
        path = Path(d) / f"{name}.log"
        path.write_text(text)
        run = subprocess.run(
            [sys.executable, str(SUMMARY), "--gate", arm, str(path)],
            capture_output=True,
            text=True,
            check=False,
        )
    return run.returncode, run.stdout + run.stderr


def both(w: float) -> str:
    return log(
        *(single(r, a, s) for r in ROUNDS for a, s in (("a", 10), ("a2", 10), ("w", w)))
    )


def main() -> None:
    cases = {
        "within 1.10x passes": (0, both(10), "within 1.10x"),
        "a real miss fails": (1, both(20), "above 1.1"),
        "no reference arm": (
            1,
            log(*(single(r, "w", 20) for r in ROUNDS)),
            "no reference arm",
        ),
        "no common round": (
            1,
            log(
                *(single(r, a, 10) for r in range(3) for a in ("a", "a2")),
                *(single(r, "w", 20) for r in range(3, 6)),
            ),
            "nothing compared",
        ),
        "no gate arm": (
            1,
            log(*(single(r, a, 10) for r in ROUNDS for a in ("a", "a2"))),
            "nothing compared",
        ),
        "no rows": (1, "start=now\n", "no rows"),
        "the deciding metric reads 0": (1, both(0), "reads 0"),
        "cross: a zero producer_cpu_ns stops the summary": (
            1,
            log(*(cross(r, a, 10, 0) for r in ROUNDS for a in ("f", "f2", "w"))),
            "producer_cpu_ns reads 0",
        ),
        "cross: measured cpu passes": (
            0,
            log(*(cross(r, a, 10, 9) for r in ROUNDS for a in ("f", "f2", "w"))),
            "within 1.10x",
        ),
    }
    bad = 0
    for name, (code, text, needle) in cases.items():
        got, out = gate(name.split(":")[0], text)
        ok = (got == 0) == (code == 0) and needle in out
        print(f"{'ok  ' if ok else 'FAIL'} {name}: exit {got}")
        if not ok:
            bad += 1
            print(out)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

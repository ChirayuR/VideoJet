#!/usr/bin/env python3
"""Compare a bench run's per-stream fps with a baseline, within the same PSRAM speed class.

  python3 tools/compare.py LABEL [--tol 0.6] [--results results/bench.jsonl] [--baseline docs/baseline-m1.jsonl]

The PSRAM timing tuning index picked at boot sets a speed class (docs/bench-log.md): 5 is the common, slower one;
4 and 10 are the faster one. Only same-class numbers compare. The baseline mean per stream comes from every ok,
non-profile record of that class in the baseline file; the run's from its own rows. Exit 0 only if every stream of
the run is within ±tol % (M2's "fps unchanged" rule)."""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLASS = {5: "slow", 4: "fast", 10: "fast"}


def per_stream(records):
    """Mean fps per stream over every row of `records`."""
    acc = {}
    for rec in records:
        for row in rec["rows"]:
            acc.setdefault(row["stream"], []).append(float(row["fps"]))
    return {s: sum(v) / len(v) for s, v in acc.items()}


def compare(run, baseline, tol):
    """(ok, report lines) for one run record against the baseline records."""
    cls = CLASS.get(run.get("psram_tuning"))
    if cls is None:
        return False, [f"{run['label']}: unknown speed class (psram_tuning={run.get('psram_tuning')})"]
    base = [r for r in baseline
            if r.get("ok") and not r.get("prof") and CLASS.get(r.get("psram_tuning")) == cls]
    if not base:
        return False, [f"no {cls}-class baseline records"]
    b, now = per_stream(base), per_stream([run])
    ok, lines = True, []
    for s in sorted(now):
        if s not in b:
            ok = False
            lines.append(f"{s}: no baseline")
            continue
        d = (now[s] - b[s]) / b[s] * 100
        good = abs(d) <= tol
        ok = ok and good
        lines.append(f"{s:24s} {now[s]:8.2f} vs {b[s]:8.2f} fps ({cls})  {d:+.2f} %  {'ok' if good else 'OUT'}")
    return ok, lines


def load(path):
    return [json.loads(x) for x in Path(path).read_text().splitlines() if x.strip()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("label")
    ap.add_argument("--tol", type=float, default=0.6)
    ap.add_argument("--results", default=ROOT / "results" / "bench.jsonl")
    ap.add_argument("--baseline", default=ROOT / "docs" / "baseline-m1.jsonl")
    a = ap.parse_args()
    runs = [r for r in load(a.results) if r["label"] == a.label]
    if not runs or not runs[-1].get("ok"):
        sys.exit(f"FAIL: no passing record labelled {a.label} in {a.results}")
    ok, lines = compare(runs[-1], load(a.baseline), a.tol)
    print("\n".join(lines))
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

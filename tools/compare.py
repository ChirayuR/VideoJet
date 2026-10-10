#!/usr/bin/env python3
"""Compare a bench run's per-stream fps with a baseline, within the same PSRAM speed class.

  python3 tools/compare.py LABEL [--tol 5 | --gain PCT] [--results results/bench.jsonl]
                             [--baseline docs/baseline-m1.jsonl]

The PSRAM timing tuning index picked at boot sets a speed class (docs/bench-log.md): 5 is the common, slower one;
4 and 10 are the faster one. Only same-class numbers compare. The baseline mean per stream comes from every ok,
non-profile record of that class in the baseline file; the run's from its own rows. Exit 0 only if the run has a row for
every baseline stream and each is within ±tol % (M2's "fps unchanged" rule), or, with --gain PCT, M3's acceptance rule
(tools/score.sh passes 0.3): every stream >= -PCT % and at least one >= +PCT %. --gain and --tol exclude each other.
The last line is the verdict, PASS or FAIL (KEEP or DROP with --gain), with exit status 0 or 1.

Cannot compare is no verdict: an unknown PSRAM index, no baseline records in the class, a run that lacks a baseline
stream or has one the baseline lacks, no passing record with that label, an unreadable file, any crash. Those print
`ERROR: why` and exit 2 (no PASS, FAIL, KEEP or DROP), so a gate can tell them from a rejection (1).

fps is exact: frames * 1e6 / total_us of the row (the printed 2-decimal token can hide 0.01 of a 0.3 % step). Rows
without total_us (M1 records) fall back to the token. The baseline is keyed by speed class and stream only, never by
ELF SHA.

The default ±5 % is ruling R1 of the M2 plan, not a noise figure. With identical decoder logic, code placement alone
moved fps between -4.3 % and +0.7 % (M2 Task 1), so while placement is unpinned the guard has to sit outside that band.
M3 step 0 pinned placement (code in IRAM: layout noise <= 0.11 %), so the scorer's rule is --gain 0.3."""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLASS = {5: "slow", 4: "fast", 10: "fast"}
TOL = 5.0  # percent, ruling R1 (see the docstring)


def row_fps(row):
    """Exact fps from the row's timing, frames * 1e6 / total_us; the printed 2-decimal token only for M1 rows."""
    if "total_us" in row:
        return int(row["frames"].split("/")[0]) * 1e6 / int(row["total_us"])
    return float(row["fps"])


def per_stream(records):
    """Mean fps per stream over every row of `records`."""
    acc = {}
    for rec in records:
        for row in rec["rows"]:
            acc.setdefault(row["stream"], []).append(row_fps(row))
    return {s: sum(v) / len(v) for s, v in acc.items()}


def compare(run, baseline, tol, gain=None):
    """(ok, report lines) for one run record against the baseline records.
    ok is True (accepted), False (rejected) or None (cannot compare: the lines starting `ERROR:` say why).
    gain=None: every stream within ±tol %. gain=G (tol is then ignored): every stream >= -G % and one >= +G %."""
    cls = CLASS.get(run.get("psram_tuning"))
    if cls is None:
        return None, [f"ERROR: {run['label']}: unknown speed class (psram_tuning={run.get('psram_tuning')})"]
    base = [r for r in baseline
            if r.get("ok") and not r.get("prof") and CLASS.get(r.get("psram_tuning")) == cls]
    if not base:
        return None, [f"ERROR: no {cls}-class baseline records"]
    b, now = per_stream(base), per_stream([run])
    if not now or set(b) - set(now):  # a run that dropped streams (or has none) must not pass on what is left
        what = "no rows" if not now else "no rows for " + ", ".join(sorted(set(b) - set(now)))
        return None, [f"ERROR: {run['label']}: {what}"]
    ok, won, bad, lines = True, False, False, []
    for s in sorted(now):
        if s not in b:
            bad = True
            lines.append(f"ERROR: {s}: no baseline")
            continue
        d = (now[s] - b[s]) / b[s] * 100
        if gain is None:
            tag = "ok" if abs(d) <= tol else "OUT"
        else:
            tag = "OUT" if d < -gain else "gain" if d >= gain else "ok"
        ok = ok and tag != "OUT"
        won = won or tag == "gain"
        lines.append(f"{s:24s} {now[s]:8.2f} vs {b[s]:8.2f} fps ({cls})  {d:+.3f} %  {tag}")
    if bad:
        return None, lines
    if gain is not None and not won:
        ok = False
        lines.append(f"no stream reaches +{gain:g} %")
    return ok, lines


def load(path):
    return [json.loads(x) for x in Path(path).read_text().splitlines() if x.strip()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("label")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--tol", type=float, help=f"every stream within ±TOL %% (default {TOL:g})")
    mode.add_argument("--gain", type=float, help="every stream >= -GAIN %% and one >= +GAIN %% (M3 acceptance)")
    ap.add_argument("--results", default=ROOT / "results" / "bench.jsonl")
    ap.add_argument("--baseline", default=ROOT / "docs" / "baseline-m1.jsonl")
    a = ap.parse_args()
    runs = [r for r in load(a.results) if r["label"] == a.label]
    if not runs or not runs[-1].get("ok"):
        print(f"ERROR: no passing record labelled {a.label} in {a.results}")
        sys.exit(2)
    ok, lines = compare(runs[-1], load(a.baseline), TOL if a.tol is None else a.tol, a.gain)
    print("\n".join(lines))
    if ok is None:
        sys.exit(2)
    print(("KEEP" if ok else "DROP") if a.gain is not None else ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # a crash is no verdict either: exit 2, not Python's 1 (a DROP to tools/score.sh)
        print(f"ERROR: {type(e).__name__}: {e}")
        sys.exit(2)

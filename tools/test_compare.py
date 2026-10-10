#!/usr/bin/env python3
"""Self-test for tools/compare.py: python3 tools/test_compare.py"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compare  # noqa: E402


def rec(label, tuning, fps, ok=True, prof=()):
    rows = [{"stream": s, "fps": f"{v:.2f}"} for s, v in fps.items()]
    return {"label": label, "psram_tuning": tuning, "ok": ok, "rows": rows, "prof": list(prof)}


BASE = [rec("b1", 5, {"a": 10.0, "b": 20.0}), rec("b2", 5, {"a": 10.0, "b": 20.0}),
        rec("b3", 10, {"a": 10.1, "b": 20.2}), rec("bp", 5, {"a": 9.0, "b": 18.0}, prof=["x"]),
        rec("bf", 5, {"a": 1.0, "b": 1.0}, ok=False)]

assert compare.compare(rec("r", 5, {"a": 10.05, "b": 19.9}), BASE, 0.6)[0]           # within ±0.6 %
assert not compare.compare(rec("r", 5, {"a": 10.1, "b": 20.0}), BASE, 0.6)[0]        # +1 %: out
assert compare.compare(rec("r", 4, {"a": 10.1, "b": 20.2}), BASE, 0.6)[0]            # fast class vs index 10
assert not compare.compare(rec("r", 7, {"a": 10.0, "b": 20.0}), BASE, 0.6)[0]        # unknown class
assert not compare.compare(rec("r", 5, {"a": 10.0, "c": 5.0}), BASE, 0.6)[0]         # stream with no baseline
assert not compare.compare(rec("r", 5, {}), BASE, 5.0)[0]                            # no rows: nothing was measured
assert not compare.compare(rec("r", 5, {"a": 10.0}), BASE, 5.0)[0]                   # a baseline stream (b) is missing
assert not compare.compare(rec("r", 5, {"a": 10.0, "b": 20.0, "c": 5.0}), BASE, 5.0)[0]  # extra stream, no baseline

# The command line: the default is ±5 % (ruling R1), --tol still works, and the exit status carries the verdict.
assert compare.TOL == 5.0
with tempfile.TemporaryDirectory() as d:
    res, base = Path(d, "res.jsonl"), Path(d, "base.jsonl")
    base.write_text("".join(json.dumps(r) + "\n" for r in BASE))
    res.write_text(json.dumps(rec("lab", 5, {"a": 10.3, "b": 20.6})) + "\n")  # +3 % on both streams, class 5

    def cli(*extra):
        cmd = [sys.executable, compare.__file__, "lab", "--results", str(res), "--baseline", str(base), *extra]
        return subprocess.run(cmd, capture_output=True, text=True).returncode

    assert cli() == 0                # inside the default ±5 %
    assert cli("--tol", "0.6") == 1  # outside an explicit ±0.6 %
print("test_compare: all passed")

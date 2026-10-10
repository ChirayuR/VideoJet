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
assert compare.compare(rec("r", 5, {"a": 10.1, "b": 20.0}), BASE, 0.6)[0] is False   # +1 %: out
assert compare.compare(rec("r", 4, {"a": 10.1, "b": 20.2}), BASE, 0.6)[0]            # fast class vs index 10
# Cannot compare is None, not a rejection (False): the command line turns it into `ERROR: why` and exit 2.
assert compare.compare(rec("r", 7, {"a": 10.0, "b": 20.0}), BASE, 0.6)[0] is None    # unknown class
assert compare.compare(rec("r", 5, {"a": 10.0, "c": 5.0}), BASE, 0.6)[0] is None     # stream with no baseline
assert compare.compare(rec("r", 5, {}), BASE, 5.0)[0] is None                        # no rows: nothing was measured
assert compare.compare(rec("r", 5, {"a": 10.0}), BASE, 5.0)[0] is None               # a baseline stream (b) is missing
assert compare.compare(rec("r", 5, {"a": 10.0, "b": 20.0, "c": 5.0}), BASE, 5.0)[0] is None  # extra stream (c)
assert compare.compare(rec("r", 4, {"a": 10.0, "b": 20.0}), BASE[:1], 5.0)[0] is None        # no fast-class baseline
for r, base in ((rec("r", 7, {"a": 10.0}), BASE), (rec("r", 5, {}), BASE), (rec("r", 4, {"a": 1.0}), BASE[:1])):
    ok, lines = compare.compare(r, base, 5.0)
    assert ok is None and lines and all(x.startswith("ERROR: ") for x in lines), lines
ok, lines = compare.compare(rec("r", 5, {"a": 10.0, "b": 20.0, "c": 5.0}), BASE, 5.0)
assert ok is None and "ERROR" not in lines[0] and lines[-1].startswith("ERROR: c: no baseline"), lines  # a and b print

# The command line: the default is ±5 % (ruling R1), --tol still works, and the exit status carries the verdict.
assert compare.TOL == 5.0
with tempfile.TemporaryDirectory() as d:
    res, base = Path(d, "res.jsonl"), Path(d, "base.jsonl")
    base.write_text("".join(json.dumps(r) + "\n" for r in BASE))
    res.write_text(json.dumps(rec("lab", 5, {"a": 10.3, "b": 20.6})) + "\n")  # +3 % on both streams, class 5

    def run(*extra, results=res):
        cmd = [sys.executable, compare.__file__, "lab", "--results", str(results), "--baseline", str(base), *extra]
        return subprocess.run(cmd, capture_output=True, text=True)

    def cli(*extra):
        return run(*extra).returncode

    def verdict(*extra):  # the last line of the output
        return run(*extra).stdout.splitlines()[-1]

    assert cli() == 0                # inside the default ±5 %
    assert cli("--tol", "0.6") == 1  # outside an explicit ±0.6 %
    assert cli("--gain", "0.3") == 0  # +3 % on both streams: floor holds, gain reached (M1-style rows: fps token)
    assert cli("--gain", "5") == 1    # +3 % never reaches +5 %
    assert cli("--gain", "0.3", "--tol", "5") == 2  # mutually exclusive (argparse usage error)
    # the last line is the verdict: PASS / FAIL with --tol (and by default), KEEP / DROP with --gain
    assert verdict() == "PASS" and verdict("--tol", "0.6") == "FAIL"
    assert verdict("--gain", "0.3") == "KEEP" and verdict("--gain", "5") == "DROP"
    # cannot compare is no verdict: an ERROR line, exit 2 and no PASS / FAIL / KEEP / DROP, in every mode
    bad = {"res7": rec("lab", 7, {"a": 10.3, "b": 20.6}),               # an index with no speed class
           "short": rec("lab", 5, {"a": 10.3}),                         # stream b missing
           "extra": rec("lab", 5, {"a": 10.3, "b": 20.6, "c": 1.0}),    # stream c has no baseline
           "other": rec("other", 5, {"a": 10.3, "b": 20.6}),            # no record labelled lab
           "failed": rec("lab", 5, {"a": 10.3, "b": 20.6}, ok=False)}   # the bench run failed
    for name, record in bad.items():
        Path(d, name).write_text(json.dumps(record) + "\n")
    for extra in ((), ("--tol", "0.6"), ("--gain", "0.3")):
        for name in bad:
            r = run(*extra, results=Path(d, name))
            out = r.stdout.splitlines()
            assert r.returncode == 2 and out[-1].startswith("ERROR: "), (extra, name, r.returncode, r.stdout)
            assert not {"PASS", "FAIL", "KEEP", "DROP"} & set(out), (extra, name, r.stdout)
    # a file that cannot be read is a crash, and a crash is no verdict either (Python's own exit status is 1)
    r = run("--baseline", str(Path(d, "missing.jsonl")))
    assert r.returncode == 2 and r.stdout.startswith("ERROR: FileNotFoundError"), (r.returncode, r.stdout, r.stderr)


def trec(label, tuning, rows):
    return {"label": label, "psram_tuning": tuning, "ok": True, "prof": [], "rows": rows}


def trow(stream, total_us, fps="0.00", frames=300):
    return {"stream": stream, "frames": f"{frames}/{frames}", "total_us": str(total_us), "fps": fps}


# exact fps from total_us: +0.310 % exact, but the 2-decimal tokens (30.09 vs 30.00) give 0.29999 %
assert compare.compare(trec("r", 5, [trow("s", 9_969_096, "30.09")]),
                       [trec("b", 5, [trow("s", 10_000_000, "30.00")])], None, gain=0.3)[0]
# --gain: one stream +11 %, the other -0.4 % -> rejected
assert not compare.compare(trec("r", 5, [trow("a", 9_000_000), trow("b", 10_040_000)]),
                           [trec("b", 5, [trow("a", 10_000_000), trow("b", 10_000_000)])], None, gain=0.3)[0]
# --gain: +0.1 % only -> rejected (no stream reaches +0.3 %)
assert not compare.compare(trec("r", 5, [trow("a", 9_990_000)]),
                           [trec("b", 5, [trow("a", 10_000_000)])], None, gain=0.3)[0]
# --gain: a rejection is False, and a stream with no baseline is None however much the others gain
assert compare.compare(trec("r", 5, [trow("a", 9_990_000)]),
                       [trec("b", 5, [trow("a", 10_000_000)])], None, gain=0.3)[0] is False
assert compare.compare(trec("r", 5, [trow("a", 9_000_000), trow("x", 9_000_000)]),
                       [trec("b", 5, [trow("a", 10_000_000)])], None, gain=0.3)[0] is None
# --gain: a +0.5 % stream with the other at -0.2 % (above the -0.3 % floor) -> accepted
assert compare.compare(trec("r", 5, [trow("a", 9_950_249), trow("b", 10_020_040)]),
                       [trec("b", 5, [trow("a", 10_000_000), trow("b", 10_000_000)])], None, gain=0.3)[0]
print("test_compare: all passed")

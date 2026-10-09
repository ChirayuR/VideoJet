#!/usr/bin/env python3
"""Reset the board, capture the bench's `VJ` lines, score them.

Run with the IDF python (it has pyserial):
  $IDF_PY tools/bench.py --label baseline [--set all]
Appends one JSON line to results/bench.jsonl and always ends with a PASS or FAIL line (exit 0 only on
PASS). PASS is the correctness floor and needs one clean boot: a single `VJ start`, every (set, stream,
run) of streams/ exactly once with frames=N/N and fail=0, no `error=`, and the end marker. Anything else
(a bad or missing field, a dropped line, a reboot, a silent board, a lost port) is a FAIL with reasons.
Self-test: python3 tools/test_bench.py"""
import argparse
import json
import math
import re
import sys
import time
from collections import Counter, deque
from contextlib import suppress
from pathlib import Path

try:
    import serial
except ImportError:  # score() and tools/test_bench.py do not need pyserial
    serial = None

ROOT = Path(__file__).resolve().parent.parent
KV = re.compile(r"(\w+)=(\S+)")
START = re.compile(r"VJ start\b")
VJ_RUNS = 2  # runs per stream: must equal VJ_RUNS in bench/main/bench_main.c
START_TIMEOUT = 30  # s from the reset to `VJ start` (a healthy boot takes about 1 s)
ROW_INTS = "run fail first_fail idr_us p_mean_us p50_us p95_us max_us int_min psram_min".split()


def kv(line):
    return dict(KV.findall(line))


def num(d, key, conv=int):
    """d[key] converted, or None when it is missing or malformed."""
    try:
        return conv(d[key])
    except (KeyError, ValueError):
        return None


def after_start(lines):
    """The capture from its first `VJ start` on: whatever came before is the previous run's output."""
    for i, line in enumerate(lines):
        if START.match(line):
            return lines[i:]
    return []


def short(keys):
    text = ", ".join("{}/{}/run{}".format(*k) for k in keys[:4])
    return text + (f" (+{len(keys) - 4} more)" if len(keys) > 4 else "")


def score(lines, expected_names, runs, which):
    """Verdict on a capture: (ok, reasons). `lines` are the `VJ` lines in arrival order,
    `expected_names` maps "search"/"heldout" to the stream names, `which` is "search" or "all".
    Only what follows the first `VJ start` counts. A bad or missing field is a reason, never an exception."""
    body = after_start(lines)
    if not body:
        return False, ["no `VJ start` line"]
    sets = ("search",) if which == "search" else ("search", "heldout")
    end = "VJ search_done" if which == "search" else "VJ done"
    expected = {(s, n, r) for s in sets for n in expected_names.get(s, ()) for r in range(runs)}
    reasons = [f"no streams/{s}/*.h264 to expect" for s in sets if not expected_names.get(s)]
    n_names = sum(len(expected_names.get(s, ())) for s in sets)
    n = num(kv(body[0]), "streams")
    if n is None:
        reasons.append("`VJ start` has no streams=<n>")
    elif (n != n_names) if which == "all" else (n < n_names):
        reasons.append(f"`VJ start` says streams={n}, streams/ has {n_names}")

    seen = Counter()
    ended = rebooted = False
    for line in body[1:]:
        if START.match(line):
            reasons.append("second `VJ start`: the board rebooted mid-run")
            rebooted = True
            break
        if line == end:
            ended = True
            break
        if not line.startswith("VJ "):  # also skips `VJP` profile lines
            continue
        if "stream=" not in line:
            if "error=" in line:
                reasons.append(f"board gave up: {line}")
            continue
        pairs = KV.findall(line)
        d = dict(pairs)
        key = (d.get("set"), d.get("stream"), num(d, "run"))
        if len(pairs) != len(d) or None in key:  # a repeated key means two rows ran together
            reasons.append(f"garbled row: {line[:100]}")
            continue
        seen[key] += 1
        tag = "{}/{}/run{}".format(*key)
        if "error" in d:  # `error=nomem|init` rows have no stats at all
            reasons.append(f"{tag}: board error={d['error']}")
            continue
        bad = [k for k in ROW_INTS if num(d, k) is None]
        fps = num(d, "fps", float)
        if fps is None or not math.isfinite(fps) or fps <= 0:
            bad.append("fps")
        frames = re.fullmatch(r"([0-9]+)/([0-9]+)", d.get("frames", ""))
        if not frames:
            bad.append("frames")
        if bad:
            reasons.append(f"{tag}: missing or bad {' '.join(bad)}")
            continue
        got, want = int(frames[1]), int(frames[2])
        if want == 0 or got != want:
            reasons.append(f"{tag}: frames={got}/{want}")
        if int(d["fail"]) != 0:
            reasons.append(f"{tag}: fail={d['fail']} first_fail={d['first_fail']}")

    have = set(seen)
    for what, keys in (("missing rows", sorted(expected - have)),
                       ("duplicate rows", sorted(k for k, c in seen.items() if c > 1)),
                       ("unexpected rows", sorted(have - expected))):
        if keys:
            reasons.append(f"{what}: {short(keys)}")
    if not ended and not rebooted:
        reasons.append(f"no `{end}`: capture incomplete")
    return not reasons, reasons


def open_port(port, tries=20):
    """Open the console, retrying while the USB device re-enumerates; the last OSError after `tries`."""
    for i in range(tries):
        try:
            return serial.Serial(port, 115200, timeout=0.5)
        except OSError:  # pyserial's SerialException is an OSError
            if i == tries - 1:
                raise
            time.sleep(0.5)


def capture(port, stop, timeout):
    """Reset the S3 via the USB-Serial-JTAG RTS line and collect its console until `stop`, the end marker.
    Returns (VJ lines, last 20 non-VJ lines, why the capture itself failed or None). Stops early on a
    second `VJ start`, a `VJ ... error=` line without stream= (the board gave up), or a ROM boot banner
    after `VJ start` (a reboot: the tail then still ends with the panic text). Gives up when `VJ start`
    has not come START_TIMEOUT s after the reset."""
    if serial is None:
        return [], [], "pyserial is not importable: run with the IDF python"
    try:
        s = open_port(port)
        s.dtr = False
        s.rts = True
        time.sleep(0.1)
        s.rts = False
    except OSError as e:
        return [], [], f"cannot open or reset {port}: {e}"
    lines, tail, buf, started, abort = [], deque(maxlen=20), b"", False, None
    t0 = time.time()
    while True:
        now = time.time()
        if now > t0 + timeout:
            abort = f"timeout: no `{stop}` within {timeout:.0f} s"
            break
        if not started and now > t0 + START_TIMEOUT:
            abort = f"no `VJ start` within {START_TIMEOUT} s of the reset"
            break
        try:
            buf += s.read(4096)
        except OSError:  # the port can drop while the chip resets: reconnect, without a reset pulse
            with suppress(OSError):
                s.close()
            buf = b""  # a line cut by the drop is garbage
            time.sleep(0.5)  # let the device re-enumerate; also keeps a flapping port from spinning
            try:
                s = open_port(port)
            except OSError as e:
                abort = f"serial port lost: {e}"
                break
            continue
        *complete, buf = buf.split(b"\n")
        finished = False
        for raw in complete:
            line = raw.decode(errors="replace").strip()
            if line.startswith("VJ"):
                print(line, flush=True)
                lines.append(line)
                if START.match(line):
                    finished = started  # a second `VJ start`: score() reports the reboot
                    started = True
                elif line.startswith("VJ ") and (line == stop or ("error=" in line and "stream=" not in line)):
                    finished = True
            elif line:
                if started and "ESP-ROM:" in line:
                    abort = "the board rebooted mid-run (ROM boot banner after `VJ start`)"
                    finished = True
                else:
                    tail.append(line)
            if finished:
                break
        if finished:
            break
    with suppress(OSError):
        s.close()
    return lines, list(tail), abort


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True)
    ap.add_argument("--set", choices=("search", "all"), default="search")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--timeout", type=float, default=1800)
    a = ap.parse_args()
    # The container is packed from these directories with the same names (tools/pack_streams.py).
    names = {s: sorted(p.stem[:23] for p in (ROOT / "streams" / s).glob("*.h264"))
             for s in ("search", "heldout")}
    lines, tail, abort = capture(a.port, "VJ search_done" if a.set == "search" else "VJ done", a.timeout)
    ok, reasons = score(lines, names, VJ_RUNS, a.set)
    if abort:
        ok = False
        reasons.insert(0, abort)
    body = after_start(lines)
    rows = [kv(x) for x in body if x.startswith("VJ ") and " stream=" in x]
    prof = [kv(x) for x in body if x.startswith("VJP ")]
    out = ROOT / "results" / "bench.jsonl"
    out.parent.mkdir(exist_ok=True)
    with out.open("a") as f:
        f.write(json.dumps({"label": a.label, "time": time.strftime("%Y-%m-%dT%H:%M:%S"), "set": a.set,
                            "ok": ok, "reasons": reasons, "rows": rows, "prof": prof}) + "\n")
    fps = {}
    for r in rows:
        v = num(r, "fps", float)
        if v is not None:
            fps.setdefault(r.get("stream", "?"), []).append(v)
    for name, v in fps.items():
        print(f"{name:24s} fps " + " ".join(f"{x:7.2f}" for x in v))
    sys.stdout.flush()
    if ok:
        print(f"PASS: {a.set}, {len(rows)} rows", flush=True)
    else:
        if tail:
            print("last non-VJ console lines:\n  " + "\n  ".join(tail), file=sys.stderr, flush=True)
        more = f" (+{len(reasons) - 8} more, all in results/bench.jsonl)" if len(reasons) > 8 else ""
        print("FAIL: " + "; ".join(reasons[:8]) + more, flush=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

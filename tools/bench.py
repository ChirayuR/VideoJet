#!/usr/bin/env python3
"""Reset the board, capture the bench's `VJ` lines, score them.

Run with the IDF python (it has pyserial):
  $IDF_PY tools/bench.py --label baseline [--set all]
Appends one JSON line to results/bench.jsonl and always ends with a PASS or FAIL line (exit 0 only on
PASS). PASS is the correctness floor and needs one clean boot: a single `VJ start`, every (set, stream,
run) of streams/ exactly once with frames=N/N and fail=0, no `error=`, and the end marker. Anything else
(a bad or missing field, a dropped line, a reboot, a silent board, a hung one, a lost port) is a FAIL with reasons.
fps shifts a little between boots with the boot log's `PSRAM timing tuning index`, so the record and the summary
carry that index as psram_tuning (null when the boot line was not seen): compare runs within one index.
Self-test: python3 tools/test_bench.py"""
import argparse
import json
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
IDLE_TIMEOUT = 180  # s without a `VJ` line after `VJ start` (the slowest row, full_map's 300 frames, takes about 55 s)
# ESP-IDF boot log: this boot's PSRAM timing calibration. The bench's fps shifts with it (docs/bench-log.md).
TUNING = re.compile(r"PSRAM timing tuning index: ([0-9]{1,6})(?![0-9])")
# A row field exactly as the firmware's printf writes it: no sign, no leading zeros, at most 19 digits (int64).
# So a dropped byte (fps=1724, fail=10 read as 0) or a stray one is a damaged row, and no int() or float() can
# be handed more than it can take. fps is never 0.00; first_fail is -1 or a frame index.
INT = re.compile(r"(?:0|[1-9][0-9]{0,18})")
FIELDS = {k: INT for k in "fail idr_us p_mean_us p50_us p95_us max_us int_min psram_min".split()}
FIELDS.update(first_fail=re.compile("-1|" + INT.pattern), frames=re.compile(INT.pattern + "/" + INT.pattern),
              fps=re.compile(r"(?!0\.00\Z)" + INT.pattern + r"\.[0-9]{2}"))


def kv(line):
    return dict(KV.findall(line))


def gave_up(line):
    """`VJ error=no_streams` and the like: a `VJ` line with error= and no stream= (the board gave up)."""
    return line.startswith("VJ ") and "error=" in line and "stream=" not in line


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
    Only what follows the first `VJ start` counts (the board gives up with `VJ error=no_streams` before one
    and never prints it after a start, so such a line is a reason only when no `VJ start` follows it).
    A row must match the firmware's format exactly (FIELDS). A bad or missing field is a reason, never an exception."""
    body = after_start(lines)
    if not body:
        return False, ["no `VJ start` line"] + [f"board gave up before `VJ start`: {x}" for x in lines if gave_up(x)]
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
        if gave_up(line):
            reasons.append(f"board gave up: {line}")
            continue
        if not line.startswith("VJ ") or "stream=" not in line:  # also skips `VJP` profile lines and markers
            continue
        pairs = KV.findall(line)
        d = dict(pairs)
        run = d.get("run", "")
        key = (d.get("set"), d.get("stream"), int(run) if INT.fullmatch(run) else None)
        if len(pairs) != len(d) or None in key:  # a repeated key means two rows ran together
            reasons.append(f"garbled row: {line[:100]}")
            continue
        seen[key] += 1
        tag = "{}/{}/run{}".format(*key)
        if "error" in d:  # `error=nomem|init` rows have no stats at all
            reasons.append(f"{tag}: board error={d['error']}")
            continue
        bad = [k for k, rx in FIELDS.items() if not rx.fullmatch(d.get(k, ""))]
        if bad:
            reasons.append(f"{tag}: missing or bad {' '.join(bad)}")
            continue
        got, want = map(int, d["frames"].split("/"))
        if want == 0 or got != want:
            reasons.append(f"{tag}: frames={got}/{want}")
        if d["fail"] != "0" or d["first_fail"] != "-1":  # a clean row says exactly this
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


def capture(port, stop, timeout, idle=IDLE_TIMEOUT):
    """Reset the S3 via the USB-Serial-JTAG RTS line and collect its console until `stop`, the end marker.
    Returns (VJ lines, last 20 non-VJ lines, why the capture itself failed or None, PSRAM tuning index or None).
    The index is the last TUNING boot-log line before the first `VJ start`. What arrives before that start is the
    boot, or the previous run's leftovers still in the pipe: a stop or error marker there is ignored. After it the
    capture stops early on a second `VJ start`, a `VJ ... error=` line without stream= (the board gave up), or a ROM
    boot banner (a reboot: the tail then still ends with the panic text). It gives up when `VJ start` has not come
    START_TIMEOUT s after the reset, or `idle` s pass without a `VJ` line after it (a hung candidate)."""
    if serial is None:
        return [], [], "pyserial is not importable: run with the IDF python", None
    try:
        s = open_port(port)
        s.dtr = False
        s.rts = True
        time.sleep(0.1)
        s.rts = False
    except OSError as e:
        return [], [], f"cannot open or reset {port}: {e}", None
    lines, tail, buf, started, abort, tuning = [], deque(maxlen=20), b"", False, None, None
    t0 = last_vj = time.time()
    while True:
        now = time.time()
        if now > t0 + timeout:
            abort = f"timeout: no `{stop}` within {timeout:.0f} s"
            break
        if not started and now > t0 + START_TIMEOUT:
            abort = f"no `VJ start` within {START_TIMEOUT} s of the reset"
            break
        if started and now > last_vj + idle:
            abort = f"no VJ output for {idle:.0f} s"
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
                last_vj = now
                if START.match(line):
                    finished = started  # a second `VJ start`: score() reports the reboot
                    started = True
                elif started and line.startswith("VJ ") and (line == stop or gave_up(line)):
                    finished = True  # before the first `VJ start` the same lines are the previous run's
            elif line:
                if started and "ESP-ROM:" in line:
                    abort = "the board rebooted mid-run (ROM boot banner after `VJ start`)"
                    finished = True
                else:
                    tail.append(line)
                    match = None if started else TUNING.search(line)
                    if match:
                        tuning = int(match[1])
            if finished:
                break
        if finished:
            break
    with suppress(OSError):
        s.close()
    return lines, list(tail), abort, tuning


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True)
    ap.add_argument("--set", choices=("search", "all"), default="search")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--timeout", type=float, default=1800)
    ap.add_argument("--idle-timeout", type=float, default=IDLE_TIMEOUT,
                    help="s without a VJ line after `VJ start` before the run counts as hung (default %(default)s)")
    a = ap.parse_args()
    # The container is packed from these directories with the same names (tools/pack_streams.py).
    names = {s: sorted(p.stem[:23] for p in (ROOT / "streams" / s).glob("*.h264"))
             for s in ("search", "heldout")}
    lines, tail, abort, tuning = capture(a.port, "VJ search_done" if a.set == "search" else "VJ done",
                                         a.timeout, a.idle_timeout)
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
                            "psram_tuning": tuning, "ok": ok, "reasons": reasons, "rows": rows, "prof": prof}) + "\n")
    fps = {}
    for r in rows:
        v = num(r, "fps", float)
        if v is not None:
            fps.setdefault(r.get("stream", "?"), []).append(v)
    for name, v in fps.items():
        print(f"{name:24s} fps " + " ".join(f"{x:7.2f}" for x in v))
    print(f"psram_tuning={tuning}", flush=True)  # the boot's PSRAM timing index: fps is only comparable within one
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

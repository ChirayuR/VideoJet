#!/usr/bin/env python3
"""Reset the board, capture the bench's `VJ` lines, score them.

Run with the IDF python (it has pyserial):
  $IDF_PY tools/bench.py --label baseline [--set all]
Appends one JSON line to results/bench.jsonl. Exit 0 only if every stream is complete,
error-free and matches every golden hash (the correctness floor)."""
import argparse
import json
import re
import sys
import time
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parent.parent
KV = re.compile(r"(\w+)=(\S+)")


def capture(port, stop, timeout):
    """Reset the S3 via the USB-Serial-JTAG RTS line; return (VJ lines, reached `stop`)."""
    s = serial.Serial(port, 115200, timeout=0.5)
    s.dtr = False
    s.rts = True
    time.sleep(0.1)
    s.rts = False
    lines, buf, deadline = [], b"", time.time() + timeout
    while time.time() < deadline:
        try:
            buf += s.read(4096)
        except serial.SerialException:  # the port can drop while the chip resets
            time.sleep(0.5)
            s = serial.Serial(port, 115200, timeout=0.5)
            continue
        *complete, buf = buf.split(b"\n")
        for raw in complete:
            line = raw.decode(errors="replace").strip()
            if line.startswith("VJ"):
                print(line, flush=True)
                lines.append(line)
                if line == stop:
                    return lines, True
    return lines, False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True)
    ap.add_argument("--set", choices=("search", "all"), default="search")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--timeout", type=float, default=1800)
    a = ap.parse_args()
    lines, finished = capture(a.port, "VJ search_done" if a.set == "search" else "VJ done", a.timeout)
    rows = [dict(KV.findall(x)) for x in lines if x.startswith("VJ ") and " stream=" in x]
    prof = [dict(KV.findall(x)) for x in lines if x.startswith("VJP ")]
    ok = finished and bool(rows) and not any("error=" in x for x in lines)
    for r in rows:
        got, want = r.get("frames", "0/1").split("/")
        ok = ok and r.get("fail") == "0" and got == want
    out = ROOT / "results" / "bench.jsonl"
    out.parent.mkdir(exist_ok=True)
    with out.open("a") as f:
        f.write(json.dumps({"label": a.label, "time": time.strftime("%Y-%m-%dT%H:%M:%S"),
                            "set": a.set, "ok": ok, "rows": rows, "prof": prof}) + "\n")
    fps = {}
    for r in rows:
        fps.setdefault(r["stream"], []).append(float(r["fps"]))
    for name, v in fps.items():
        print(f"{name:24s} fps " + " ".join(f"{x:7.2f}" for x in v))
    if not finished:
        print("bench: did not reach the end marker (timeout)", file=sys.stderr)
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

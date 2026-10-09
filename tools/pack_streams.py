#!/usr/bin/env python3
"""Pack streams/{search,heldout}/*.h264 + .golden into build-host/streams.bin (the `streams` partition).

Layout, little-endian: b"VJS1", u32 count, count x <24s5I> {name, set, data_off, data_len, frames,
golden_off}, then 16-byte-aligned blobs. Golden = frames x u32 (vj_hash per frame).
Must match vj_entry_t in bench/main/bench_main.c."""
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PART_SIZE = 0xCF0000  # partitions.csv


def align(x):
    return (x + 15) & ~15


def main():
    items = []
    for set_id, sub in ((0, "search"), (1, "heldout")):
        for h264 in sorted((ROOT / "streams" / sub).glob("*.h264")):
            golden = h264.with_suffix(".golden")
            if not golden.exists():
                raise SystemExit(f"missing golden for {h264}")
            hashes = [int(x, 16) for x in golden.read_text().split()]
            items.append((h264.stem[:23].encode(), set_id, h264.read_bytes(),
                          struct.pack(f"<{len(hashes)}I", *hashes), len(hashes)))
    off = align(8 + 44 * len(items))
    head_len = off
    entries, blobs = b"", b""
    for name, set_id, data, gold, frames in items:
        d_off = off
        g_off = align(d_off + len(data))
        off = align(g_off + len(gold))
        entries += struct.pack("<24s5I", name, set_id, d_off, len(data), frames, g_off)
        blobs += data.ljust(g_off - d_off, b"\0") + gold.ljust(off - g_off, b"\0")
    out = (b"VJS1" + struct.pack("<I", len(items)) + entries).ljust(head_len, b"\0") + blobs
    if len(out) > PART_SIZE:
        raise SystemExit(f"streams.bin is {len(out)} bytes; the partition holds {PART_SIZE}")
    (ROOT / "build-host").mkdir(exist_ok=True)
    (ROOT / "build-host" / "streams.bin").write_bytes(out)
    print(f"{len(items)} streams, {len(out)} bytes")


if __name__ == "__main__":
    main()

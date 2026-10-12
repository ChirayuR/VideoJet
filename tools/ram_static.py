#!/usr/bin/env python3
"""Internal RAM that libvideojet.a occupies statically, from an IDF linker map: usage ram_static.py BUILD/videojet_bench.map
Sums every input section of libvideojet.a objects placed in .iram0.text (code), .dram0.data (tables, plus the string literals and
other rodata that sit in DRAM because the code does) and .dram0.bss. With the heap high-water mark of the decoder (the bench's
int_used, which includes the worker task's stack) this is the library's internal-RAM total against the 100,000 B budget."""
import re
import sys

SECTIONS = (".iram0.text", ".dram0.data", ".dram0.bss")
ENTRY = re.compile(r"^\s*(\S+)?\s+0x[0-9a-f]+\s+(0x[0-9a-f]+)\s+(\S+)$")  # [name] addr size object
NAME_ONLY = re.compile(r"^\s(\S+)\s*$")


def main(path):
    total = dict.fromkeys(SECTIONS, 0)
    cur = None
    pending = None
    for line in open(path, errors="replace"):
        if line and line[0] == ".":  # an output section starts in column 0
            cur = line.split()[0] if line.split()[0] in SECTIONS else None
            pending = None
            continue
        if cur is None:
            continue
        if line and line[0] not in " \t":
            cur = None
            continue
        m = NAME_ONLY.match(line)
        if m:  # a long input section name: the numbers are on the next line
            pending = True
            continue
        m = ENTRY.match(line)
        if m and "libvideojet.a(" in m.group(3):
            total[cur] += int(m.group(2), 16)
        pending = None
    for s in SECTIONS:
        print(f"{s:12s} {total[s]:7d}")
    print(f"{'static total':12s} {sum(total.values()):7d}")


if __name__ == "__main__":
    main(sys.argv[1])

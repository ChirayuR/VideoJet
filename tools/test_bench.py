#!/usr/bin/env python3
"""Self-test for tools/bench.py, the scorer that gates every optimisation: score() on synthetic captures
in the firmware's exact `VJ` format, and capture() against a scripted fake port and clock.
Plain asserts, no hardware, no frameworks: python3 tools/test_bench.py"""
import contextlib
import io

import bench

NAMES = {"search": ["aux_card", "aux_map"], "heldout": ["BA1_Sony_D"]}
RUNS = 2
# A real line from the m1-smoke run on the board.
REAL = ("VJ run=0 set=search stream=aux_card frames=300/300 fail=0 first_fail=-1 idr_us=116465 "
        "p_mean_us=57824 p50_us=57407 p95_us=59956 max_us=83010 fps=17.24 int_min=354655 psram_min=6810676")


def row(s, n, r, **over):
    f = dict(frames="300/300", fail="0", first_fail="-1", idr_us="116465", p_mean_us="57824", p50_us="57407",
             p95_us="59956", max_us="83010", fps="17.24", int_min="354655", psram_min="6810676")
    f.update(over)
    return f"VJ run={r} set={s} stream={n} " + " ".join(f"{k}={v}" for k, v in f.items())


def good(which="all"):
    """A clean run as the firmware prints it: per set, run-major, then the set's marker."""
    out = ["VJ start streams=3"]
    for s in ("search", "heldout")[: 1 if which == "search" else 2]:
        out += [row(s, n, r) for r in range(RUNS) for n in NAMES[s]]
        out.append("VJ search_done" if s == "search" else "VJ done")
    return out


def fail(lines, text, which="all"):
    """The capture must FAIL, with a reason containing `text`, and not raise."""
    ok, reasons = bench.score(lines, NAMES, RUNS, which)
    assert not ok and any(text in r for r in reasons), reasons


def test_row_format_is_the_firmwares():
    assert row("search", "aux_card", 0) == REAL


def test_complete_capture_passes():
    assert bench.score(good(), NAMES, RUNS, "all") == (True, [])
    assert bench.score(good("search"), NAMES, RUNS, "search") == (True, [])


def test_stale_bytes_before_the_first_start_are_ignored():
    stale = [row("search", "aux_map", 1, fail="9"), "VJ search_done", "VJ done"]
    assert bench.score(stale + good(), NAMES, RUNS, "all") == (True, [])


def test_dropped_row_fails():
    lines = good()
    lines.remove(row("search", "aux_map", 1))
    fail(lines, "missing rows: search/aux_map/run1")


def test_no_held_out_rows_under_all_fails():
    fail([x for x in good() if "set=heldout" not in x], "missing rows: heldout/BA1_Sony_D/run0")


def test_second_start_fails():
    fail(good()[:3] + good(), "second `VJ start`")  # a partial boot, a reboot, then a full run


def test_no_start_fails():
    fail([row("search", "aux_card", 0), row("search", "aux_map", 0), "VJ search_done"], "no `VJ start`", "search")


def test_error_row_without_stats_fails_without_raising():
    lines = good()
    lines[1] = "VJ run=0 set=search stream=aux_card error=nomem"
    fail(lines, "search/aux_card/run0: board error=nomem")


def test_error_after_a_full_row_fails():
    lines = good()
    lines[1] += " error=decode"
    fail(lines, "board error=decode")


def test_missing_end_marker_fails():
    fail(good()[:-1], "no `VJ done`")


def test_frames_mismatch_fails():
    for bad in ("299/300", "0/0", "abc", "300", "300/"):
        lines = good()
        lines[1] = row("search", "aux_card", 0, frames=bad)
        fail(lines, "search/aux_card/run0")


def test_hash_failure_fails():
    lines = good()
    lines[1] = row("search", "aux_card", 0, fail="3", first_fail="17")
    fail(lines, "fail=3 first_fail=17")


def test_bad_numbers_fail():
    for field, bad in (("fps", "inf"), ("fps", "nan"), ("fps", "0.00"), ("fps", "x"), ("fail", "x"), ("idr_us", "")):
        lines = good()
        lines[1] = row("search", "aux_card", 0, **{field: bad})
        fail(lines, "search/aux_card/run0")


def test_board_gave_up_fails():
    fail(["VJ start streams=3", "VJ error=no_streams"], "board gave up: VJ error=no_streams", "search")
    fail(["VJ error=no_streams"], "no `VJ start`", "search")


def test_duplicate_and_unexpected_rows_fail():
    lines = good()
    lines.insert(2, lines[1])
    fail(lines, "duplicate rows: search/aux_card/run0")
    for extra in (row("search", "aux_card", 2), row("search", "other", 0)):
        lines = good()
        lines.insert(2, extra)
        fail(lines, "unexpected rows")


def test_stream_count_in_the_start_line():
    for n, which, ok in ((3, "all", True), (4, "all", False), (2, "all", False), (11, "search", True), (1, "search", False)):
        lines = good(which)
        lines[0] = f"VJ start streams={n}"
        assert bench.score(lines, NAMES, RUNS, which)[0] == ok, (n, which)
    lines = good()
    lines[0] = "VJ start"
    fail(lines, "no streams=<n>")


def test_truncated_rows_never_pass_and_never_raise():
    full = row("search", "aux_card", 0)
    last = full.index("psram_min=") + len("psram_min=")
    for k in range(len(full)):
        lines = good()
        lines[1] = full[:k]
        ok = bench.score(lines, NAMES, RUNS, "all")[0]
        assert not ok or k > last, k  # only digits lost from the last, unscored value can slip through


def test_two_rows_run_together_fail():
    lines = good()
    lines[1:3] = [lines[1] + lines[2]]
    fail(lines, "garbled row")


def test_nothing_to_expect_fails():
    assert not bench.score(good(), {"search": [], "heldout": []}, RUNS, "all")[0]


class Board:
    """Stands in for pyserial and the clock (bench.serial, bench.time): scripted reads, and time that only
    moves when the code sleeps or a read times out. A script item is bytes, or an exception to raise."""

    def __init__(self, script, fail=lambda n: False):
        self.script, self.fail, self.now, self.opens = list(script), fail, 1000.0, 0

    def time(self):
        return self.now

    def sleep(self, s):
        self.now += s

    def Serial(self, port, baud, timeout):  # fail(n): does the n-th open raise?
        self.opens += 1
        if self.fail(self.opens):
            raise OSError("no such device")
        return self

    def read(self, n):
        if not self.script:
            self.now += 0.5  # a real read() waits out its 0.5 s timeout
            return b""
        item = self.script.pop(0)
        if isinstance(item, Exception):
            raise item
        return item

    def close(self):
        pass


def cap(script, stop="VJ search_done", timeout=1800, **kw):
    board = Board(script, **kw)
    saved = bench.serial, bench.time
    bench.serial = bench.time = board
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            lines, tail, abort = bench.capture("fake", stop, timeout)
    finally:
        bench.serial, bench.time = saved
    return lines, tail, abort, board


def test_capture_reassembles_lines_and_stops_at_the_end_marker():
    lines, tail, abort, board = cap([b"boot noise\r\nVJ sta", b"rt streams=2\nVJ searc", b"h_done\n", b"VJ later\n"])
    assert lines == ["VJ start streams=2", "VJ search_done"] and tail == ["boot noise"] and abort is None
    assert board.script == [b"VJ later\n"]  # it stopped reading


def test_capture_stops_at_a_second_start():
    lines, _, abort, board = cap([b"VJ start streams=2\nVJ start streams=2\n", b"VJ search_done\n"])
    assert lines == ["VJ start streams=2"] * 2 and abort is None and board.script
    fail(lines, "second `VJ start`", "search")


def test_capture_stops_when_the_board_gives_up_but_not_on_a_stream_error():
    lines, _, _, board = cap([b"VJ start streams=2\nVJ error=no_streams\n", b"more\n"])
    assert lines[-1] == "VJ error=no_streams" and board.script == [b"more\n"]
    lines, _, _, _ = cap([b"VJ start streams=2\nVJ run=0 set=search stream=a error=nomem\nVJ search_done\n"])
    assert lines[-1] == "VJ search_done"


def test_capture_gives_up_on_a_silent_board_after_30_s():
    lines, tail, abort, board = cap([b"ESP-ROM:esp32s3-20210327\nrst:0x15\n"])
    assert lines == [] and "30 s" in abort and tail == ["ESP-ROM:esp32s3-20210327", "rst:0x15"]
    assert 30 < board.now - 1000.0 < 40


def test_capture_times_out_without_the_end_marker():
    _, _, abort, board = cap([b"VJ start streams=2\n"], timeout=100)
    assert "timeout" in abort and 100 < board.now - 1000.0 < 102


def test_capture_keeps_the_panic_text_across_a_reboot():
    boot = b"".join(b"I (%d) boot: line\n" % i for i in range(30))
    panic = b"Guru Meditation Error: Core 0 panic'ed (LoadProhibited)\nBacktrace: 0x4200abcd\nRebooting...\n"
    _, tail, abort, _ = cap([b"VJ start streams=2\n", panic, b"ESP-ROM:esp32s3-20210327\n" + boot])
    assert "rebooted" in abort and len(tail) == 3 and tail[0].startswith("Guru") and tail[-1] == "Rebooting..."


def test_capture_ignores_the_banner_of_its_own_reset():
    lines, _, abort, _ = cap([b"ESP-ROM:esp32s3-20210327\nVJ start streams=2\nVJ search_done\n"])
    assert abort is None and lines == ["VJ start streams=2", "VJ search_done"]


def test_capture_reconnects_and_drops_the_cut_line():
    lines, _, abort, board = cap([b"VJ start streams=2\nVJ run=0 set=sea", OSError("dropped"), b"VJ search_done\n"])
    assert lines == ["VJ start streams=2", "VJ search_done"] and abort is None and board.opens == 2


def test_capture_port_retries_are_bounded():
    _, _, abort, board = cap([b"VJ start streams=2\nVJ search_done\n"], fail=lambda n: n <= 3)  # USB still enumerating
    assert abort is None and board.opens == 4
    _, _, abort, board = cap([b"VJ start streams=2\n", OSError("dropped")], fail=lambda n: n > 1)  # never comes back
    assert "serial port lost" in abort and board.opens == 21
    _, _, abort, board = cap([], fail=lambda n: True)
    assert "cannot open" in abort and board.opens == 20


if __name__ == "__main__":
    tests = [f for name, f in list(globals().items()) if name.startswith("test_")]
    for t in tests:
        t()
        print("ok  ", t.__name__)
    print(f"{len(tests)} tests passed")

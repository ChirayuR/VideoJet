#!/usr/bin/env python3
"""Self-test for tools/bench.py, the scorer that gates every optimisation: score() on synthetic captures
in the firmware's exact `VJ` format, capture() against a scripted fake port and clock, and main() with
capture() faked. Plain asserts, no hardware, no frameworks: python3 tools/test_bench.py"""
import contextlib
import io
import json
import sys
import tempfile
from pathlib import Path

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


def wire(lines):
    """What the console delivers for these lines."""
    return "".join(x + "\n" for x in lines).encode()


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


def test_a_pre_start_error_is_carried_into_the_reasons():
    fail(["VJ error=no_streams"], "board gave up before `VJ start`: VJ error=no_streams", "search")
    fail(["VJ error=no_streams"], "no `VJ start`", "search")  # and still says there was no start
    fail(["VJ run=0 set=search stream=a error=nomem", "VJ error=no_streams"], "VJ error=no_streams", "search")
    # ahead of a complete clean run it is the previous boot's, like any other stale line
    assert bench.score(["VJ error=no_streams"] + good("search"), NAMES, RUNS, "search") == (True, [])


def test_fps_is_exactly_what_the_firmware_prints():
    # %.2f of a positive double. A dropped byte (fps=1724, 17.2) or a stray one must not pass as a speed.
    for bad in ("1724", "17.2", "17.244", "17.", ".24", "+17.24", "-17.24", "1e1", "17.24x", "017.24", "0.00",
                "١٧.٢٤", "1٧.٢٤"):
        lines = good()
        lines[1] = row("search", "aux_card", 0, fps=bad)
        fail(lines, "search/aux_card/run0: missing or bad fps")
    for ok in ("0.01", "9.73", "17.24", "100.00"):
        lines = good()
        lines[1] = row("search", "aux_card", 0, fps=ok)
        assert bench.score(lines, NAMES, RUNS, "all") == (True, []), ok


def test_integer_fields_are_plain_integers():
    for field in "fail idr_us p_mean_us p50_us p95_us max_us int_min psram_min".split():
        for bad in ("+5", "-5", "5.0", "1_000", "0x1f", "5e3", "007", "00", "٥", "1٥", "5x", "9" * 20):
            lines = good()
            lines[1] = row("search", "aux_card", 0, **{field: bad})
            fail(lines, f"search/aux_card/run0: missing or bad {field}")


def test_first_fail_is_minus_one_or_an_index_and_agrees_with_fail():
    for bad in ("-2", "+1", "-0", "01", "1.5", "x", "9" * 20):
        lines = good()
        lines[1] = row("search", "aux_card", 0, first_fail=bad)
        fail(lines, "search/aux_card/run0: missing or bad first_fail")
    # A clean row says fail=0 first_fail=-1. fail=10 first_fail=17 with a byte dropped must not read as clean.
    for f, ff in (("0", "17"), ("0", "0"), ("3", "-1")):
        lines = good()
        lines[1] = row("search", "aux_card", 0, fail=f, first_fail=ff)
        fail(lines, f"fail={f} first_fail={ff}")


def test_absurdly_long_numbers_fail_without_raising():
    # int() refuses more than 4300 digits and float() turns them into inf: neither may escape as an error or a pass.
    long = "9" * 5000
    for field, bad in (("frames", long + "/300"), ("frames", "300/" + long), ("fail", long), ("first_fail", long),
                       ("idr_us", long), ("fps", long + ".00")):
        lines = good()
        lines[1] = row("search", "aux_card", 0, **{field: bad})
        fail(lines, "search/aux_card/run0")
    for bad in ("+1", "01", "1.0", "x", "٠", long):  # the run number is held to the same format
        lines = good()
        lines[1] = row("search", "aux_card", bad)
        fail(lines, "garbled row")


class Board:
    """Stands in for pyserial and the clock (bench.serial, bench.time): scripted reads, and time that only
    moves when the code sleeps or a read times out. A script item is bytes (b"" is a read that times out,
    see silence()), or an exception to raise. Counts open()s and close()s."""

    def __init__(self, script, fail=lambda n: False):
        self.script, self.fail, self.now, self.opens = list(script), fail, 1000.0, 0
        self.closes, self.closes_at_open = 0, []

    def time(self):
        return self.now

    def sleep(self, s):
        self.now += s

    def Serial(self, port, baud, timeout):  # fail(n): does the n-th open raise?
        self.opens += 1
        if self.fail(self.opens):
            raise OSError("no such device")
        self.closes_at_open.append(self.closes)  # handles closed by the time this one opened
        return self

    def read(self, n):
        item = self.script.pop(0) if self.script else b""
        if isinstance(item, Exception):
            raise item
        if not item:
            self.now += 0.5  # a real read() waits out its 0.5 s timeout
        return item

    def close(self):
        self.closes += 1


def silence(s):
    """Script items for `s` seconds of nothing on the line."""
    return [b""] * int(s / 0.5)


def run(script, stop="VJ search_done", timeout=1800, fail=lambda n: False, **limits):
    """capture() on a fake board: (its result, the board). `limits` (idle=) go to capture() only when given."""
    board = Board(script, fail)
    saved = bench.serial, bench.time
    bench.serial = bench.time = board
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            result = bench.capture("fake", stop, timeout, **limits)
    finally:
        bench.serial, bench.time = saved
    return result, board


def cap(script, **kw):
    """run(), keeping capture()'s first three results: lines, non-VJ tail, why it gave up; and the board."""
    (lines, tail, abort, *_), board = run(script, **kw)
    return lines, tail, abort, board


def psram_tuning(script, **kw):
    """capture()'s fourth result."""
    (_, _, _, tuning), _ = run(script, **kw)
    return tuning


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


def test_capture_gives_up_early_with_the_same_result_shape():
    result, _ = run([], fail=lambda n: True)  # the port never opens
    assert result == ([], [], "cannot open or reset fake: no such device", None)
    saved, bench.serial = bench.serial, None  # no pyserial
    try:
        assert bench.capture("fake", "VJ search_done", 10) == ([], [], "pyserial is not importable: run with the IDF python",
                                                               None)
    finally:
        bench.serial = saved


def test_capture_reconnect_closes_the_old_handle_before_reopening():
    _, _, abort, board = cap([b"VJ start streams=2\nVJ run=0 set=sea", OSError("dropped"), b"VJ search_done\n"])
    assert abort is None and board.opens == 2
    assert board.closes_at_open == [0, 1]  # the dropped handle was closed before the next one opened
    assert board.closes == 2  # and the last one on the way out
    _, _, _, board = cap([b"VJ start streams=2\nVJ search_done\n"])
    assert board.opens == 1 and board.closes == 1


def test_capture_ignores_markers_left_over_from_before_its_own_start():
    stale = b"VJ search_done\nVJ error=no_streams\nVJ done\n"  # the previous run's tail, still in the pipe
    for which, stop in (("search", "VJ search_done"), ("all", "VJ done")):
        lines, _, abort, board = cap([stale, wire(good(which))], stop=stop)
        assert abort is None and not board.script, which  # it read on to the real end
        assert bench.score(lines, NAMES, RUNS, which) == (True, []), which
    _, _, abort, board = cap([stale])  # and with no start at all it waits for one, then gives up on that
    assert "no `VJ start` within 30 s" in abort and board.now - 1000.0 > 30


TUNING = b"I (84) MSPI Timing: PSRAM timing tuning index: %d\n"
START_DONE = b"VJ start streams=2\nVJ search_done\n"


def test_capture_returns_the_psram_tuning_index_from_the_boot_log():
    assert psram_tuning([b"I (353) MSPI Timing: PSRAM timing tuning index: 10\n" + START_DONE]) == 10
    assert psram_tuning([b"\x1b[0;32mI (84) MSPI Timing: PSRAM timing tuning index: 4\x1b[0m\r\n" + START_DONE]) == 4
    cut = [b"I (84) MSPI Timing: PSRAM timing tun", b"ing index: 5\n" + START_DONE]  # a line cut across two reads
    assert psram_tuning(cut) == 5


def test_capture_psram_tuning_is_none_without_the_boot_line():
    for boot in (b"", b"I (84) esp_psram: Speed: 80MHz\n", b"I (84) MSPI Timing: Flash timing tuning index: 3\n",
                 b"I (84) MSPI Timing: PSRAM timing tuning index: x\n",  # nothing it can trust or raise on
                 b"I (84) MSPI Timing: PSRAM timing tuning index: " + b"9" * 5000 + b"\n"):
        assert psram_tuning([boot + START_DONE]) is None, boot[:60]


def test_capture_psram_tuning_is_the_last_line_before_the_start():
    boot = TUNING % 4 + TUNING % 5 + b"VJ start streams=2\n"
    assert psram_tuning([boot + TUNING % 7 + b"VJ search_done\n"]) == 5  # what follows the start is not the boot's
    assert psram_tuning([b"VJ start streams=2\n" + TUNING % 7 + b"VJ search_done\n"]) is None


def test_capture_gives_up_when_the_board_goes_quiet_after_start():
    _, _, abort, board = cap([b"VJ start streams=2\n"] + silence(400))
    assert abort == "no VJ output for 180 s" and 180 < board.now - 1000.0 < 182  # the default
    _, _, abort, board = cap([b"VJ start streams=2\n"] + silence(400), idle=60)
    assert abort == "no VJ output for 60 s" and 60 < board.now - 1000.0 < 62
    _, _, abort, board = cap([b"I (84) boot: x\n"] + silence(100), idle=10)  # pre-start: START_TIMEOUT only
    assert abort == "no `VJ start` within 30 s of the reset" and 30 < board.now - 1000.0 < 32


def test_capture_gaps_shorter_than_the_idle_limit_still_pass():
    for limits, gap in (({}, 170), ({"idle": 60}, 50)):  # about as long a silence as each limit allows
        script = [wire(good("search")[:1])]
        for x in good("search")[1:]:
            script += silence(gap) + [wire([x])]
        lines, _, abort, board = cap(script, **limits)
        assert abort is None and board.now - 1000.0 > 5 * gap - 1, limits  # in all far longer than the limit
        assert bench.score(lines, NAMES, RUNS, "search") == (True, []), limits


def test_capture_console_noise_is_not_vj_output():
    script = [b"VJ start streams=2\n"] + [b"Task watchdog got triggered\n", b""] * 400
    _, tail, abort, board = cap(script)
    assert abort == "no VJ output for 180 s" and board.now - 1000.0 < 182 and tail[-1] == "Task watchdog got triggered"


def run_main(lines, tuning, abort=None, *argv):
    """bench.main() with capture() faked and a scratch repo root: (exit code, stdout, jsonl records, capture's args)."""
    with tempfile.TemporaryDirectory() as tmp:
        root, calls = Path(tmp), []
        for s, names in NAMES.items():
            (root / "streams" / s).mkdir(parents=True)
            for n in names:
                (root / "streams" / s / f"{n}.h264").touch()

        def fake_capture(*args):
            calls.append(args)
            return lines, ["last console line"], abort, tuning

        saved = bench.ROOT, bench.capture, sys.argv
        bench.ROOT, bench.capture, sys.argv = root, fake_capture, ["bench.py", "--label", "t", *argv]
        out, code = io.StringIO(), None
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
                try:
                    bench.main()
                except SystemExit as e:
                    code = e.code
        finally:
            bench.ROOT, bench.capture, sys.argv = saved
        records = [json.loads(x) for x in (root / "results" / "bench.jsonl").read_text().splitlines()]
    return code, out.getvalue(), records, calls


def test_main_records_and_prints_the_psram_tuning_index():
    code, out, records, calls = run_main(good("search"), 5, None, "--idle-timeout", "7")
    assert code == 0 and records[-1]["ok"] and records[-1]["psram_tuning"] == 5
    assert "psram_tuning=5" in out and out.strip().splitlines()[-1].startswith("PASS")
    assert calls[0][3] == 7  # --idle-timeout reaches capture()
    code, out, records, calls = run_main(good("search"), None)
    assert code == 0 and records[-1]["psram_tuning"] is None and "psram_tuning=None" in out
    assert calls[0][3] == 180  # the default
    code, out, records, _ = run_main(good("search")[:-1], 4, "no VJ output for 180 s")  # a FAIL records it too
    assert code == 1 and not records[-1]["ok"] and records[-1]["psram_tuning"] == 4 and "psram_tuning=4" in out


def test_main_carries_a_pre_start_no_streams_error_into_the_jsonl():
    code, out, records, _ = run_main(["VJ error=no_streams"], 4, "no `VJ start` within 30 s of the reset")
    assert code == 1 and not records[-1]["ok"]
    assert any("VJ error=no_streams" in r for r in records[-1]["reasons"]), records[-1]["reasons"]
    assert "VJ error=no_streams" in out


if __name__ == "__main__":
    tests = [f for name, f in list(globals().items()) if name.startswith("test_")]
    for t in tests:
        t()
        print("ok  ", t.__name__)
    print(f"{len(tests)} tests passed")

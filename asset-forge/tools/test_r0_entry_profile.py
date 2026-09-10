import csv
import tempfile
import unittest
from pathlib import Path

from analyze_r0_entry_profile import analyze, PREFIX, STAGES, COUNTERS

BASE = {"Entry": 5.0, "Footprint": 3.0, "Memo": 2.0, "Compute": 1.5, "Resolve": 1.0,
        "Sky": 0.5, "Nearest": 1.0, "Prefetch": 0.2}


def write_csv(path, frames, extra_columns=()):
    columns = (["EVENTS", "FrameTime"] + [PREFIX + s + "Ms" for s in STAGES]
               + [PREFIX + s + "Calls" for s in STAGES] + [PREFIX + c for c in COUNTERS] + list(extra_columns))
    with path.open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(columns)
        for ms, calls, counters, extra in frames:
            writer.writerow(["", "16.6"] + [str(ms.get(s, 0.0)) for s in STAGES]
                            + [str(calls.get(s, 0)) for s in STAGES]
                            + [str(counters.get(c, 0)) for c in COUNTERS] + list(extra))


class R0EntryProfileTests(unittest.TestCase):
    def frame(self, **overrides):
        ms = dict(BASE)
        ms.update(overrides)
        return (ms, {s: 1 for s in STAGES},
                {"ZCells": 4, "Evaluations": 3, "MemoHits": 2, "EpochInvalidations": 0}, ())

    def test_nested_profile_summarised(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "frames.csv"
            idle = ({}, {}, {}, ())
            write_csv(p, [idle, self.frame(), self.frame(Entry=7.0, Footprint=4.0), idle])
            r = analyze(p)
            self.assertTrue(r["nesting_ok"])
            self.assertEqual(r["frames_total"], 4)
            self.assertEqual(r["frames_with_entry"], 2)
            self.assertEqual(r["stages"]["Entry"]["calls"], 2)
            self.assertEqual(r["stages"]["Entry"]["per_frame_max_ms"], 7.0)
            self.assertEqual(r["stages"]["Footprint"]["total_ms"], 7.0)
            self.assertEqual(r["counters"]["ZCells"], 8)
            self.assertAlmostEqual(r["entry_residual_ms"]["total"], (5 - 3 - 1) + (7 - 4 - 1))
            self.assertEqual(r["missing_series"], [])

    def test_absent_profile_refused(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "frames.csv"
            with p.open("w", newline="") as s:
                w = csv.writer(s)
                w.writerow(["EVENTS", "FrameTime", "VoxelStream/TickMs"])
                w.writerow(["", "16.6", "1"])
            with self.assertRaisesRegex(ValueError, "absent"):
                analyze(p)

    def test_negative_sample_refused(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "frames.csv"
            write_csv(p, [self.frame(Resolve=-0.1)])
            with self.assertRaisesRegex(ValueError, "Invalid"):
                analyze(p)

    def test_per_thread_duplicate_refused(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "frames.csv"
            ms, calls, counters, _ = self.frame()
            write_csv(p, [(ms, calls, counters, ("9.9",))], extra_columns=(PREFIX + "EntryMs",))
            with self.assertRaisesRegex(ValueError, "Ambiguous"):
                analyze(p)

    def test_nesting_violation_flagged_not_summed(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "frames.csv"
            write_csv(p, [self.frame(), self.frame(Resolve=1.8)])  # resolve exceeds compute by 0.3 ms
            r = analyze(p, tolerance_ms=0.05)
            self.assertFalse(r["nesting_ok"])
            v = r["nesting_violations"]["resolve<=compute"]
            self.assertEqual(v["frames"], 1)
            self.assertAlmostEqual(v["worst_excess_ms"], 0.3)
            self.assertEqual(r["nesting_violations"]["compute<=memo"]["frames"], 0)
            self.assertTrue(analyze(p, tolerance_ms=0.5)["nesting_ok"])

    def test_log_window_lines_counted_only(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "frames.csv"
            log = Path(d) / "game.log"
            write_csv(p, [self.frame()])
            log.write_text("x\nR0EntryProfile inclusive slice-window-max ms: entry=1 footprint=2\n"
                           "R0EntryProfile inclusive slice-window-max ms: entry=3\n")
            self.assertEqual(analyze(p, log)["log_window_lines"], 2)


if __name__ == "__main__":
    unittest.main()

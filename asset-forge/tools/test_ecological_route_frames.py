import copy
import csv
from pathlib import Path
import tempfile
import unittest

from analyze_ecological_route_frames import analyze_frames, frame_rows


class RouteFrameEvidenceTest(unittest.TestCase):
    def setUp(self):
        self.log = "\n".join((
            "VoxelRoute PROFILE_BEGIN mono=100.000000 captureFrame=0",
            "VoxelRoute WALK_BEGIN point=0 mono=100.020000",
            "VoxelRoute WALK_END arrived=0 mono=100.080000",
            "VoxelRoute PROFILE_END mono=101.080000 elapsed=1.080000 captureFrame=5",
            "VoxelRoute PROFILE_SAVED ok=1 path=route-frames.csv"))
        self.rows = [dict(zip(("FrameTime", "GPUTime", "VoxelRoute/CaptureFrame", "VoxelRoute/ElapsedSeconds", "VoxelRoute/Walking", "VoxelRoute/Point"), values)) for values in (
            (20, 100, 0, .02, 0, 0), (20, 5, 1, .04, 1, 0),
            (20, 7, 2, .06, 1, 0), (20, 100, 3, .08, 0, 0),
            (1000, 100, 4, 1.08, 0, 1))]

    def run_capture(self, rows=None, log=None):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "frames.csv"
            rows = self.rows if rows is None else rows
            with path.open("w", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
            return analyze_frames(path, self.log if log is None else log)

    def test_excludes_screenshot_and_transition_costs(self):
        report = self.run_capture()
        self.assertEqual(report["walking"]["frames"], 1)
        self.assertEqual(report["boundary_rows_excluded"], 1)
        self.assertEqual(report["walking"]["metrics_ms"]["FrameTime"]["max"], 20)
        self.assertEqual(report["walking"]["metrics_ms"]["GPUTime"]["median"], 7)
        self.assertEqual(report["csv_frames"], 5)

    def test_ue_expanded_final_header_and_late_series(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "frames.csv"
            initial = list(self.rows[0])
            expanded = initial + ["VoxelStream/LateMs", "Ignored", "Ignored"]
            with path.open("w", newline="") as stream:
                writer = csv.writer(stream)
                writer.writerow(initial)
                for i, row in enumerate(self.rows):
                    writer.writerow(list(row.values()) + ([4, 0, 0] if i >= 2 else []))
                writer.writerow(expanded)
                writer.writerow(["[HasHeaderRowAtEnd]", "1"])
            report = analyze_frames(path, self.log)
            self.assertEqual(list(frame_rows(path))[1]["VoxelStream/LateMs"], "0")
            self.assertEqual(report["walking"]["metrics_ms"]["VoxelStream/LateMs"]["median"], 4)
            with path.open("a", newline="") as stream:
                csv.writer(stream).writerow(expanded + ["FrameTime"])
            with self.assertRaisesRegex(ValueError, "duplicate measured"):
                analyze_frames(path, self.log)

    def test_duplicate_worker_substages_excluded_not_overwritten_or_summed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "frames.csv"
            header = list(self.rows[0]) + ["VoxelStream/AppearanceCanonicalMs"] * 2 + ["VoxelStream/SubmitMs"]
            with path.open("w", newline="") as stream:
                writer = csv.writer(stream); writer.writerow(header)
                for row in self.rows:
                    writer.writerow(list(row.values()) + [111, 222, 3])
            report = analyze_frames(path, self.log)
            self.assertNotIn("VoxelStream/AppearanceCanonicalMs", report["walking"]["metrics_ms"])
            self.assertEqual(report["walking"]["metrics_ms"]["VoxelStream/SubmitMs"]["max"], 3)
            self.assertEqual(report["walking"]["metrics_ms"]["GPUTime"]["median"], 7)
            self.assertEqual(report["csv_column_diagnostics"]["ambiguous_columns_excluded"]["VoxelStream/AppearanceCanonicalMs"], [6, 7])
            with path.open("a", newline="") as stream:
                csv.writer(stream).writerow(header + ["VoxelStream/SubmitMs"])
            with self.assertRaisesRegex(ValueError, "duplicate measured primary"):
                analyze_frames(path, self.log)

    def test_rejects_false_or_incomplete_attribution(self):
        for name, mutate in (
            ("duplicate_frame", lambda r: r[2].update({"VoxelRoute/CaptureFrame": 1})),
            ("phase_outside_event", lambda r: r[4].update({"VoxelRoute/Walking": 1, "VoxelRoute/Point": 0})),
            ("nonfinite_timing", lambda r: r[2].update({"GPUTime": "nan"})),
            ("clock_mismatch", lambda r: r[4].update({"FrameTime": 5000})),
            ("backwards_clock", lambda r: r[2].update({"VoxelRoute/ElapsedSeconds": .01})),
            ("corrupt_duration", lambda r: r[2].update({"FrameTime": "invalid"})),
            ("missing_middle_labels", lambda r: r[2].update({"VoxelRoute/CaptureFrame": ""})),
        ):
            with self.subTest(name=name):
                rows = copy.deepcopy(self.rows)
                mutate(rows)
                with self.assertRaises(ValueError):
                    self.run_capture(rows=rows)
        with self.assertRaises(ValueError):
            self.run_capture(log=self.log.replace("PROFILE_SAVED", "NOT_SAVED"))
        with self.assertRaises(ValueError):
            self.run_capture(log=self.log.replace("VoxelRoute PROFILE_END", "VoxelRoute WALK_BEGIN point=1 mono=100.200000\nVoxelRoute WALK_END arrived=1 mono=100.800000\nVoxelRoute PROFILE_END"))


if __name__ == "__main__":
    unittest.main()

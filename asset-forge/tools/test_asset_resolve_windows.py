import unittest
from analyze_asset_resolve_windows import predictive_windows


class PredictiveCountersTest(unittest.TestCase):
    def test_changed_epoch_is_separate_terminal_outcome(self):
        prefix = "[2026.09.10-04.33.48:457]LogVoxelPerf: Voxel predictive asset resolve (window): "
        suffix = " probes=10 nonresident=0 queueRemaining=0 tickMs=1 maxTickMs=.5 queueBuilds=1 queueCells=20 queueBuildMs=.4 maxQueueBuildMs=.4 launchCap=8 inFlightCap=32 probeCap=256 queueCap=2048"
        # One task crosses a reporting window; epoch rejection is separate
        # from residency-at-landing rejection in the source counters.
        first = prefix + "launched=4 landed=1 raced=0 rejected=1 epochRejected=1 pending=1" + suffix
        second = prefix + "launched=0 landed=1 raced=0 rejected=0 epochRejected=0 pending=0" + suffix
        self.assertEqual(len(predictive_windows(first + "\n" + second)), 2)
        with self.assertRaises(ValueError):
            predictive_windows(first.replace("pending=1", "pending=0"))
        with self.assertRaises(ValueError):
            predictive_windows(first.replace("queueRemaining=0", "queueRemaining=2049"))
        with self.assertRaises(ValueError):
            predictive_windows(first.replace("tickMs=1", "tickMs=nan"))


if __name__ == "__main__":
    unittest.main()

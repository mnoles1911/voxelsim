import csv
import json
from pathlib import Path
import tempfile
import unittest
from analyze_ecological_route import foundation_evidence


class FoundationEvidenceTest(unittest.TestCase):
    def test_complete_columns_and_reject_corrupted_support_claims(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            route = {"foundation": {"min_x_voxel": -25, "min_y_voxel": -25, "plane_z_voxel": 0},
                     "waypoints": [{"x_m": 0, "y_m": 0, "arrival_radius_m": .35}]}
            manifest = dict(routeSha256="a" * 64, configurationSha256="b" * 64, speciesManifestSha256="c" * 64)
            summary = dict(schema=1, voxel_mm=100, width_mm=5000, clearance_mm=3000,
                           max_fill_mm_provisional=500, bearing_inspection_mm=1000,
                           route_sha256="a" * 64, configuration_sha256="b" * 64, species_manifest_sha256="c" * 64,
                           min_x_voxel=-25, min_y_voxel=-25, plane_z_voxel=0, pawn_x_m=0, pawn_y_m=0,
                           column_count=2500, unknown_columns=0, wet_columns=0, obstructed_columns=0,
                           provisional_criteria_columns=2500, complete_known_survey=True, approved_build_site=False)
            columns = [dict(x_voxel=x, y_voxel=y, ground_found=1, first_ground_z_voxel=-1,
                            fill_gap_mm=0, bearing_thickness_mm=1000, first_void_depth_mm=-1,
                            material_unknown=0, water_known=1, water_mm=0, overhead_occupied_voxels=0,
                            non_ground_below_plane_voxels=0, provisional_criteria=1)
                       for y in range(-25, 25) for x in range(-25, 25)]
            def write():
                (root / "foundation-summary.json").write_text(json.dumps(summary))
                with (root / "foundation-columns.csv").open("w", newline="") as handle:
                    writer = csv.DictWriter(handle, fieldnames=columns[0].keys())
                    writer.writeheader(); writer.writerows(columns)
            write()
            self.assertEqual(foundation_evidence(root, route, manifest)["provisional_criteria_columns"], 2500)
            for obj, key, value in ((columns[-1], "water_mm", 100), (columns[-1], "fill_gap_mm", 100),
                                    (columns[-1], "x_voxel", 23), (summary, "pawn_x_m", 1),
                                    (summary, "unknown_columns", 1), (summary, "route_sha256", "d" * 64)):
                with self.subTest(key=key):
                    old = obj[key]; obj[key] = value; write()
                    with self.assertRaises(ValueError): foundation_evidence(root, route, manifest)
                    obj[key] = old


if __name__ == "__main__":
    unittest.main()

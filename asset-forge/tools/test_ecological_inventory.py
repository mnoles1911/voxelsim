"""Checks for the placement inventory's identity/publication boundary."""
import json
from pathlib import Path
import struct
import tempfile
import unittest

from ecological_inventory import compile_inventory, digest, geometry
from compile_ecological_placement import compile_rules


def voxel_blob():
    return b"VXA1" + struct.pack("<IiiiIIIIIII", 3, -1, -1, 0, 2, 2, 2, 100, 1, 0, 0) + struct.pack("<BI", 16, 8)


class InventoryTests(unittest.TestCase):
    def test_measured_geometry_and_invalid_runs(self):
        blob = voxel_blob()
        self.assertEqual(geometry(blob)["height_mm"], 200)
        self.assertEqual(geometry(blob)["bounds_radius_mm"], 100)
        with self.assertRaises(ValueError):
            geometry(blob[:-4] + struct.pack("<I", 7))
        with self.assertRaises(ValueError):
            geometry(blob + b"trailing")

    def test_endorsement_and_exact_bank_identity_are_required(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "library" / "oak" / "oak-0007"
            source.mkdir(parents=True)
            (source.parent / "species.json").write_text(json.dumps({"baseline_spec": {"kind": "tree"}}))
            blob = voxel_blob()
            (source / "tree.vxa").write_bytes(blob)
            meta = {"id": "oak-0007", "species": "oak", "seed": 7, "kind": "tree",
                    "visual_approved": True, "review_status": "endorsed"}
            (source / "meta.json").write_text(json.dumps(meta))
            engine = root / "out" / "engine"
            bank = engine / "banks" / "oak" / "oak-0007.vxa"
            bank.parent.mkdir(parents=True)
            bank.write_bytes(blob)
            publication = engine / "appearance" / "published.json"
            publication.parent.mkdir(parents=True)
            publication.write_text(json.dumps({"models": [{"id": "oak-0007", "species": "oak",
                "seed": 7, "geometry_sha256": digest(blob)}]}))
            result = compile_inventory(root)
            self.assertEqual(result["variant_count"], 1)
            self.assertEqual(result["variants"][0]["asset_seed"], 7)
            self.assertNotIn("seedIndex", result["variants"][0])
            trait = {"species": "oak", "kind": "tree", "community_weights_per_mille": {"woodland": 1000},
                     "crown_opacity_per_mille": 700, "trunk_exclusion_mm": 800}
            trait_path = source.parent / "ecology.json"
            trait_path.write_text(json.dumps(trait))
            (root / "rules").mkdir()
            (root / "rules" / "temperate-ecology.json").write_text(json.dumps({
                "revision": "test", "biomes": ["temperate_forest"], "fields": {},
                "communities": [{"id": "woodland"}], "species_traits": ["library/oak/ecology.json"]}))
            compiled = compile_rules(root)
            self.assertEqual(compiled['algorithm_version'],2)
            self.assertEqual(compiled["readiness"]["published_variants"], 1)
            self.assertEqual(compiled["readiness"]["unrepresented_communities"], [])
            self.assertEqual(compiled, compile_rules(root))
            preview = dict(result, preview_only=True)
            preview["variants"][0]["growth_form"] = "woodland"
            self.assertEqual(compile_rules(root, preview_inventory=preview)["profiles"][0]["variants"][0]["growth_form"], "woodland")
            preview["variants"][0]["growth_form"] = "unknown-shape"
            with self.assertRaisesRegex(ValueError, "Unknown growth form"):
                compile_rules(root, preview_inventory=preview)
            trait["community_weights_per_mille"]["woodland"] = 1001
            trait_path.write_text(json.dumps(trait))
            with self.assertRaises(ValueError):
                compile_rules(root)
            bank.write_bytes(blob[:-1] + b"\x01")
            self.assertEqual(compile_inventory(root)["variant_count"], 0)
            bank.write_bytes(blob)
            meta["review_status"] = "rejected"
            (source / "meta.json").write_text(json.dumps(meta))
            result = compile_inventory(root)
            self.assertEqual(result["variant_count"], 0)
            self.assertIn("not explicitly endorsed", result["refused"][0]["reason"])


if __name__ == "__main__":
    unittest.main()

import copy
import json
from pathlib import Path
import tempfile
import unittest
from analyze_authored_lod_bake import inspect


class AuthoredBakeTests(unittest.TestCase):
    def test_aliases_require_complete_matching_source_proof(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest, log = Path(directory) / "bake.json", Path(directory) / "bake.log"
            model = {"id": "grass-1", "object_path": "/Game/Shared.Mesh",
                     "mesh_facts": {"lods": [{"triangles": 100}, {"triangles": 76}]}}
            data = {"schema": 2, "settings": "schema=2;authoredLOD=1", "models": [model, dict(model, id="grass-2")]}
            proof = "DetailAuthoredLOD preserved mesh=/Game/Shared.Mesh lods=2 authored/builtTriangles: L0=100/100 L1=76/76\n"
            complete = "DetailBake complete: 2 meshes;\n"
            manifest.write_text(json.dumps(data)); log.write_text(proof + complete)
            self.assertEqual(len(inspect(manifest, log)["models"]), 2)
            for invalid in (complete, proof, proof + proof + complete,
                            proof.replace("76/76", "76/50") + complete,
                            proof.replace("L1=", "L2=") + complete):
                log.write_text(invalid)
                with self.assertRaises(ValueError): inspect(manifest, log)
            log.write_text(proof + complete)
            tampered = copy.deepcopy(data)
            tampered["models"][1]["mesh_facts"]["lods"][1]["triangles"] = 50
            manifest.write_text(json.dumps(tampered))
            with self.assertRaises(ValueError): inspect(manifest, log)


if __name__ == "__main__": unittest.main()

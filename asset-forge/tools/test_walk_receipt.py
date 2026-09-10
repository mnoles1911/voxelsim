import copy
import json
import tempfile
import unittest
from pathlib import Path
from validate_walk_receipt import validate_receipt, sha, MODULES, INPUTS

class ReceiptTests(unittest.TestCase):
    def test_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = {k: 'a'*64 for k in INPUTS}
            manifest['runtimeModuleHashes'] = {k: 'b'*64 for k in MODULES}
            (root/'run-manifest.json').write_text(json.dumps(manifest))
            for name in ('game.log', 'frames.csv'):
                (root/name).write_text('captured data')
            good = dict(schema=1, status='passed', processExitCode=0, failure=None,
                        competingProcesses=[], manifestSha256=sha(root/'run-manifest.json'),
                        endRuntimeModuleHashes=manifest['runtimeModuleHashes'],
                        endInputHashes={k: manifest[k] for k in INPUTS},
                        artifactHashes={k: sha(root/k) for k in ('game.log', 'frames.csv')})
            receipt = root/'run-validation.json'
            with self.assertRaises(ValueError): validate_receipt(root)
            receipt.write_text(json.dumps(good));validate_receipt(root)
            for key, value in [('status','running'), ('status','failed'), ('processExitCode',1),
                               ('failure','Runtime module changed'), ('competingProcesses',[{'id':42}]),
                               ('manifestSha256','c'*64), ('endInputHashes',{}), ('artifactHashes',{}),
                               ('endRuntimeModuleHashes',{})]:
                bad = copy.deepcopy(good);bad[key] = value
                receipt.write_text(json.dumps(bad))
                with self.subTest(key=key, value=value), self.assertRaises(ValueError): validate_receipt(root)
            bad = copy.deepcopy(good)
            bad['endRuntimeModuleHashes']['UnrealEditor-VoxelEarth.dll'] = 'c'*64
            receipt.write_text(json.dumps(bad))
            with self.assertRaisesRegex(ValueError, 'Runtime module changed'): validate_receipt(root)
            receipt.write_text(json.dumps(good))
            (root/'frames.csv').write_text('tampered')
            with self.assertRaisesRegex(ValueError, 'artifact'): validate_receipt(root)

if __name__ == '__main__': unittest.main()

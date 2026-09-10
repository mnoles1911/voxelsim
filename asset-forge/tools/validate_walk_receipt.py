"""Require a completed capture and bind its validation to the actual artifacts."""
import hashlib
import json
from pathlib import Path

MODULES = {'UnrealEditor-VoxelEarth.dll', 'UnrealEditor-VoxelEarthShaders.dll',
           'UnrealEditor-VoxelEarthUI.dll'}
INPUTS = ('configurationSha256', 'speciesManifestSha256', 'detailCacheManifestSha256')

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def validate_receipt(root):
    root = Path(root)
    try:
        receipt = json.loads((root / 'run-validation.json').read_text(encoding='utf-8-sig'))
        manifest = json.loads((root / 'run-manifest.json').read_text(encoding='utf-8-sig'))
    except (OSError, ValueError) as exc:
        raise ValueError('Missing or unreadable capture validation receipt') from exc
    if (receipt.get('schema') != 1 or receipt.get('status') != 'passed'
            or receipt.get('processExitCode') != 0 or receipt.get('failure')
            or receipt.get('competingProcesses') != []):
        raise ValueError('Capture did not pass exclusive runtime validation')
    if receipt.get('manifestSha256', '').lower() != sha(root / 'run-manifest.json'):
        raise ValueError('Capture manifest changed after validation')
    start = manifest.get('runtimeModuleHashes', {})
    end = receipt.get('endRuntimeModuleHashes', {})
    if set(start) != MODULES or set(end) != MODULES:
        raise ValueError('Missing required runtime module pins')
    if {k: v.lower() for k, v in start.items()} != {k: v.lower() for k, v in end.items()}:
        raise ValueError('Runtime module changed during capture')
    for key in INPUTS:
        if not manifest.get(key) or receipt.get('endInputHashes', {}).get(key, '').lower() != manifest[key].lower():
            raise ValueError('Input changed during capture: ' + key)
    for name in ('game.log', 'frames.csv'):
        if not (root / name).is_file() or receipt.get('artifactHashes', {}).get(name, '').lower() != sha(root / name):
            raise ValueError('Missing or changed validated artifact: ' + name)
    return receipt

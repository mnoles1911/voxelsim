"""Identify the Python server source loaded from this checkout."""
import hashlib
import json
import os
from pathlib import Path


def snapshot():
    root = Path(__file__).resolve().parents[1]
    digest = hashlib.sha256()
    for path in sorted((root / 'forge').rglob('*.py')):
        digest.update(path.relative_to(root).as_posix().encode('utf8') + b'\0')
        digest.update(path.read_bytes() + b'\0')
    return dict(application='asset-forge', root=str(root),
                fingerprint=digest.hexdigest(), pid=os.getpid())


if __name__ == '__main__':
    print(json.dumps(snapshot()))

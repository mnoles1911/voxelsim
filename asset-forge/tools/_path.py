"""Put the repo root on sys.path so these scripts import `forge` from anywhere.

They live one directory down from the package, so running them as
`python tools/sheet.py` from the project root would otherwise fail -- Python
puts the SCRIPT's directory on the path, not the working directory.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

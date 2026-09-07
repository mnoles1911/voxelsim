"""Validate shared weather capture and visible foliage coverage/motion changes.

Image measurements are visual smoke checks, not GPU timings or exact motion
vectors: temporal antialiasing also changes pixels between captures.
"""
import json
from pathlib import Path
import re
import numpy as np
from PIL import Image

root = Path(__file__).resolve().parents[1]
log = (root / 'Saved/vegetation-game.log').read_text(errors='replace')
assert 'VegetationCapture COMPLETE restoredWeather=1' in log
assert 'Failed to compile Material' not in log
rows = re.findall(r'VegetationCapture SHOT (\S+) (\S+) sharedWind=\(([-\d.]+),([-\d.]+)\) valid=([\d.]+)', log)
assert len(rows) == 20, f'Expected 20 shots, got {len(rows)}'
images = root / 'ue-project/Saved/Screenshots/Vegetation'
metrics = {}
for asset, label, north, east, valid in rows:
    assert float(valid) == 1
    expected = 6 if label.startswith('east') else -6 if label == 'west' else 0
    assert abs(float(north)) < .01 and abs(float(east)-expected) < .01
    path = images / f'{asset}-{label}.png'
    a = np.asarray(Image.open(path).convert('RGB'), dtype=float)
    r, g, b = a[...,0], a[...,1], a[...,2]
    green = (g > 1.2*r) & (g > 1.2*b) & (g > 25)
    y, x = np.nonzero(green)
    metrics.setdefault(asset, {})[label] = dict(green_pixels=len(x),
        green_center_x=float(x.mean()) if len(x) else None,
        green_center_y=float(y.mean()) if len(y) else None)
for asset in ('temperate-oak', 'bramble-thicket'):
    m = metrics[asset]
    ratio = m['cutout-still']['green_pixels']/m['opaque-still']['green_pixels']
    assert ratio < .98, f'{asset}: cutout not visibly different at normal viewing distance'
    mean_east = (m['east-a']['green_center_x'] + m['east-b']['green_center_x'])/2
    assert mean_east-m['west']['green_center_x'] > 1, f'{asset}: wind reversal not visible'
    m['cutout_coverage_ratio'] = ratio
result = dict(shots=20, shared_wind_verified=True, weather_restored=True, images=metrics)
(root / 'Saved/vegetation-capture-validation.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))

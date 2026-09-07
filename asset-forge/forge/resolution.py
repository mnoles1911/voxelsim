"""Supported authoring pitches and category eligibility, in centimetres.

Terrain-stamped trees/rocks retain the world's 100 mm cell size. Environment
detail supports 100/50/25 mm; creatures and craftables may also use 12.5 mm.
"""
import math
from . import categories, kinds

TIERS_CM = ('10', '5', '2.5', '1.25')

def allowed(spec):
    category = categories.of(spec)
    if category in ('creature', 'craftable'):
        return TIERS_CM
    kind = kinds.BY_KEY.get(spec.get('kind', 'tree'))
    if kind and kind.lattice == 'terrain':
        return ('10',)
    return TIERS_CM[:3]

def normalize(spec, value):
    """Choose the next coarser allowed tier for legacy/invalid authoring input."""
    choices=allowed(spec)
    try:
        cm=float(value)
        if not math.isfinite(cm) or cm<=0: raise ValueError()
    except (ValueError,TypeError):
        cm=5.0
    return next((c for c in reversed(choices) if float(c)>=cm),choices[0])

def require(spec, value):
    cm=float(value)
    if not math.isfinite(cm) or cm not in tuple(map(float,allowed(spec))):
        raise ValueError(f"Voxel pitch {cm*10:g} mm is not allowed for {categories.of(spec)} / {spec.get('kind')}; allowed: {', '.join(str(float(c)*10) for c in allowed(spec))} mm")
    return cm

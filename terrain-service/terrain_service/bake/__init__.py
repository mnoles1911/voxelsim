"""Phase 2 geomorphic bake (`docs/terrain-amplification-plan.md`, Layer 1).

Stages B0-B3 live in sibling modules; this package only re-exports them. Nothing
here may import numba or scipy at module scope: CI runs the terrain-service test
suite on a plain `requirements.txt` environment (flask/numpy/pytest), and a
module that fails to import takes the whole job down rather than skipping. The
heavy kernels are compiled on first call instead -- see `flow._jit`.
"""

from .flow import accumulate_mfd, d8_receivers, fill_depressions

__all__ = ["accumulate_mfd", "d8_receivers", "fill_depressions"]

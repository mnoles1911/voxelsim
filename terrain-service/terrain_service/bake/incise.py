"""B2e — stream-power incision depth.

`depth = K * A^m * S^n`, the detachment-limited stream-power law. It is the pass that turns
a flow field into landform: without it the drainage network exists in the accumulation
array and nowhere in the ground.

**K is the single most important knob in the whole bake, and none of the summary statistics
distinguish a good value from a useless one — only a hillshade does.** Measured on a real
tile at 3.75 m/px:

| K      | mean incision | p99   | reads as                                          |
|--------|---------------|-------|---------------------------------------------------|
| 0.0004 | 0.00 m        | 0.01 m| a network in the flow field and nowhere else      |
| 0.012  | 0.13 m        | 0.63 m| gentle modulation; visible but not landform       |
| 0.05   | 0.56 m        | 2.61 m| valleys emerging                                  |
| 0.15   | 1.66 m        | 7.84 m| **properly dissected hillslope**                  |

Hence `K = 0.15` as the default. The plausible-looking first guess of 4e-4 was ~400x too
small and every aggregate statistic it produced looked perfectly reasonable. Phase 2 proper
should calibrate K against real DEM drainage density rather than by eye.

numpy only; no scipy, no numba.
"""

from __future__ import annotations

import numpy as np

__all__ = ["stream_power"]

# Slope floor. Flow routing hands back S == 0 on filled pits and flats; without a floor the
# `S**n` term is 0**0.8 == 0 there, which is fine, but the epsilon also keeps the function
# strictly monotone in S at the origin, which is what the monotonicity test asserts.
_SLOPE_EPS = 1e-6


def stream_power(acc: np.ndarray, slope: np.ndarray, K: float = 0.15,
                 m: float = 0.45, n: float = 0.8, cap_m: float = 25.0) -> np.ndarray:
    """Incision depth in **metres**, to be subtracted from the depression-filled surface.

    `acc`   upslope contributing area A, in m^2 (the MFD accumulation field).
    `slope` local slope S, dimensionless rise/run.
    `cap_m` depth cap. Over-carving is its own failure mode — a single cell with an
            enormous catchment and a steep local slope would otherwise punch a shaft
            through the tile — so the depth is clamped rather than left unbounded.

    Returns float32, non-negative, `<= cap_m` everywhere, and monotone non-decreasing in
    both A and S.
    """
    a = np.asarray(acc, dtype=np.float32)
    s = np.asarray(slope, dtype=np.float32)
    if a.shape != s.shape:
        raise ValueError(f"acc {a.shape} and slope {s.shape} must have the same shape")
    if K < 0.0:
        raise ValueError(f"K must be non-negative, got {K}")
    if cap_m <= 0.0:
        raise ValueError(f"cap_m must be positive, got {cap_m}")

    # Negative area or slope is meaningless and would make the power terms NaN; clamping is
    # cheaper than trusting every upstream producer.
    a = np.clip(a, 0.0, None)
    s = np.clip(s, 0.0, None) + np.float32(_SLOPE_EPS)

    depth = np.float32(K) * np.power(a, np.float32(m), dtype=np.float32) \
        * np.power(s, np.float32(n), dtype=np.float32)
    return np.minimum(depth, np.float32(cap_m))

"""Deterministic water distance from a complete set of final tile wet masks.

This is a post-bake operation: callers supply the water actually published by
each tile, not independently recomputed apron lakes. No missing neighbor is
treated as dry. Results retain the existing min-pool/floor wire convention.
"""
from __future__ import annotations

import math
import numpy as np
from .placement import _pool
from ..tile_codec import PLACEMENT_DIST_LSB_M, PLACEMENT_DIST_UNKNOWN, PLACEMENT_SUBSAMPLE


def distance_from_final_masks(masks, center, *, cell_m):
    """Return the center distance plane, requiring all nine immutable masks.

    The one-tile halo must exceed the representable distance. Keys are integer
    world tile coordinates; array rows increase with world y. The caller must
    pin mask provenance and publish under a new bake identity after all tiles
    are ready. This function never reads or writes a live cache.
    """
    if not math.isfinite(cell_m) or cell_m<=0:raise ValueError('Invalid cell size')
    cx,cy=center
    required={(cx+dx,cy+dy) for dy in (-1,0,1) for dx in (-1,0,1)}
    if not required.issubset(masks):raise ValueError('Complete neighbor wet masks required')
    shape=np.asarray(masks[center]).shape
    if len(shape)!=2 or shape[0]!=shape[1] or shape[0]%PLACEMENT_SUBSAMPLE:
        raise ValueError('Square subsample-aligned masks required')
    edge=shape[0]
    # One extra pixel conservatively covers rounding at the saturation bound.
    halo=math.ceil(PLACEMENT_DIST_UNKNOWN*PLACEMENT_DIST_LSB_M/cell_m)+1
    if halo>edge:raise ValueError('One neighbor tile cannot cover distance range')
    wet=np.empty((edge+2*halo,edge+2*halo),dtype=bool)
    dest=[slice(0,halo),slice(halo,halo+edge),slice(halo+edge,None)]
    src=[slice(edge-halo,edge),slice(None),slice(0,halo)]
    for yi,dy in enumerate((-1,0,1)):
        for xi,dx in enumerate((-1,0,1)):
            a=np.asarray(masks[(cx+dx,cy+dy)])
            if a.shape!=shape or a.dtype!=np.bool_:raise ValueError('Incompatible wet mask')
            wet[dest[yi],dest[xi]]=a[src[yi],src[xi]]
    sub=edge//PLACEMENT_SUBSAMPLE
    if not wet.any():return np.full((sub,sub),PLACEMENT_DIST_UNKNOWN,np.uint8)
    from scipy.ndimage import distance_transform_edt
    distance=distance_transform_edt(~wet)[halo:halo+edge,halo:halo+edge]*cell_m
    q=np.floor(_pool(distance,PLACEMENT_SUBSAMPLE,'min')/PLACEMENT_DIST_LSB_M)
    return np.minimum(q,PLACEMENT_DIST_UNKNOWN).astype(np.uint8)

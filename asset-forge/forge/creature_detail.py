"""Surface-following anatomical marks, shared by the animal generators.

Marks replace occupied surface materials only: they cannot disconnect geometry
or create untagged voxels, and their size follows anatomy at finer pitches.
"""
import numpy as np


def side_disk(mat, region, x, z, radius, material):
    """Project a circular mark onto BOTH curved flanks, one column at a time."""
    nx, _, nz = mat.shape
    radius = max(0.5, float(radius))
    for xx in range(max(0, int(x-radius)), min(nx, int(x+radius)+2)):
        for zz in range(max(0, int(z-radius)), min(nz, int(z+radius)+2)):
            if (xx-x)**2 + (zz-z)**2 > radius**2:
                continue
            ys = np.flatnonzero(region[xx, :, zz])
            if ys.size:
                # Don't paint a buried part through the surface of another.
                occupied = np.flatnonzero(mat[xx, :, zz])
                for y in (int(ys[0]), int(ys[-1])):
                    if y == occupied[0] or y == occupied[-1]:
                        mat[xx, y, zz] = material


def side_seam(mat, region, points, material):
    for x, z in points:
        side_disk(mat, region, round(x), round(z), 0.5, material)

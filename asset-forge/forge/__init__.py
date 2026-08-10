"""asset-forge: cubic-voxel environment asset generation for voxelsim.

Trees first. The pipeline is spec -> skeleton -> voxels, all in metres until
the last step, and every asset is reproducible from (spec, seed).

See `docs/tree-asset-generator-plan.md` in the repo root for what this is for
and `docs/tree-asset-generator-research.md` for why it is built this way.
"""

__version__ = "0.1.0"

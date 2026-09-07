"""Read-only description of every Forge environment asset's runtime contract.

This reports source facts and required capabilities; it does not claim that
production renderer ownership, physics or harvesting are already integrated.
Classification stays separate from the geometry hashes used for baking.
"""
from __future__ import annotations

from decimal import Decimal
import hashlib
import json
import re

from . import categories, kinds, manifest, resolution, spec as sm

VERSION = 1
PROFILES = {
    "tree": {"family": "woody_vegetation", "wind": "rooted_wood_and_foliage",
             "interaction": "chop_and_harvest", "fracture": "structural_wood"},
    "bush": {"family": "woody_vegetation", "wind": "rooted_wood_and_foliage",
             "interaction": "cut_and_harvest", "fracture": "woody_stems"},
    "rock": {"family": "mineral", "wind": "none",
             "interaction": "mine_and_harvest", "fracture": "mineral"},
    "grass": {"family": "flexible_vegetation", "wind": "rooted_blades",
              "interaction": "cut_and_harvest", "fracture": "none"},
    "reed": {"family": "flexible_vegetation", "wind": "rooted_stems",
             "interaction": "cut_and_harvest", "fracture": "none"},
    "flower": {"family": "flexible_vegetation", "wind": "rooted_stems_and_blooms",
               "interaction": "pick_and_harvest", "fracture": "none"},
}
COMMON_CAPABILITIES = (
    "arbitrary_catalog_identity", "exact_voxel_pitch", "local_material_grid",
    "transformed_surface_mesh", "distance_lod", "material_aware_wind_profile",
    "local_voxel_query", "local_edit_revision", "versioned_source_snapshot",
    "stable_instance_identity", "streaming_residency", "authoritative_replication",
)


def describe(name: str, body: dict) -> dict:
    """Describe one validated environment spec without changing it."""
    if categories.of(body) != "environment":
        raise ValueError(f"{name}: category is not environment")
    kind = sm.get(body, "kind")
    if kind not in kinds.BY_KEY:
        raise ValueError(f"{name}: unknown generator kind {kind!r}")
    if not name or any(c in name for c in ('/', '\\', '\0')):
        raise ValueError("asset identity must be a nonempty spec filename stem")
    cm = resolution.require(body, sm.get(body, "resolution_cm"))
    pitch = Decimal(str(cm)) * 10000
    if pitch != pitch.to_integral_value():
        raise ValueError(f"{name}: voxel pitch must be exact micrometres")
    height = manifest.nominal_height_m(body, kind)
    terrain = manifest.is_terrain_lattice(kind, height, name)
    report = manifest.ExportReport()
    layer = manifest.assign_layer(kind, height, report, name,
                                  float(sm.get(body, "placement.spacing_m")))
    profile = PROFILES.get(kind)
    concerns = []
    if profile is None:
        # A category override can use another generator. Geometry remains
        # generic, but gameplay policy needs an explicit profile decision.
        profile = {"family": "generic_environment", "wind": "none",
                   "interaction": "explicit_profile_required", "fracture": "none"}
        concerns.append("environment override needs an explicit gameplay profile")
    if kind not in manifest.KINDS_ON_SCATTER:
        concerns.append("generator kind is not admitted by current environment scatter")
    if terrain and int(pitch) != 100000:
        concerns.append("terrain manifest route requires a 100 mm source")
    if not terrain and int(pitch) == 100000:
        concerns.append("detail bank admission excludes terrain-pitch grids")
    concerns.extend(reason for _name, reason in report.unplaceable)
    classification = {"category": "environment", "kind": kind, "profile": profile}
    return {
        "contract_version": VERSION,
        "asset_id": f"forge:environment:{name}",
        "spec_name": name,
        "category": "environment",
        "generator_kind": kind,
        "category_source": categories.source_of(body),
        "family": profile["family"],
        "voxel_pitch_um": int(pitch),
        "allowed_authoring_pitches_um": [int(Decimal(c) * 10000) for c in resolution.allowed(body)],
        "spec_hash": sm.spec_hash(body),
        "seed_hash": sm.seed_hash(body),
        "classification_hash": hashlib.sha256(json.dumps(classification, sort_keys=True).encode()).hexdigest(),
        "required_capabilities": list(COMMON_CAPABILITIES),
        "recommended_profile": dict(profile),
        "current_manifest": {
            "terrain_lattice": terrain,
            "layer": layer,
            "nominal_height_m": height,
            "collision": "terrain_voxel" if terrain else "none",
            "renderer_route": "terrain_composition" if terrain else "detail_instance_mesh",
            "cover_volume_pitch_compatible": not terrain and int(pitch) == 50000,
        },
        "admission_concerns": concerns,
    }


def runtime_descriptor(entry: dict, source_md5: str, seed_index: int,
                       catalog_hash: str = "", provider_hash: str = "",
                       *, fellable: bool = False) -> dict:
    """Map to FVoxelEnvironmentAssetDescriptor's current generic schema.

    The caller supplies the actual VXA MD5; this helper never substitutes a
    spec hash for a content hash. Pitch/dimensions live in the decoded grid.
    """
    if entry.get("category") != "environment" or not re.fullmatch(r"[0-9a-fA-F]{32}", source_md5):
        raise ValueError("environment descriptor requires the source VXA MD5")
    if not isinstance(seed_index, int) or isinstance(seed_index, bool) or not 0 <= seed_index <= 0xffffffff:
        raise ValueError("bank seed index must fit uint32")
    return {"SpecId": entry["spec_name"], "Kind": entry["generator_kind"],
            "Category": "environment", "SourceHash": source_md5.lower(),
            "SpecHash": entry["spec_hash"], "CatalogHash": catalog_hash,
            "ProviderHash": provider_hash, "SeedIndex": seed_index,
            "Fellable": bool(fellable), "Legacy": False}

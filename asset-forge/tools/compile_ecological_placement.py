"""Compile authored traits and exact published inventory into runtime input.

Only this output joins species rules to approved geometry. It does not activate
the runtime or change any endorsement. Missing community representation is reported.
"""
from pathlib import Path
import argparse
import json
from ecological_inventory import ROOT, compile_inventory, digest

# Must match voxelcore/assetecology.h. Version 2 includes terrain-conditioned
# irregular stands and species-specific water-distance response.
ALGORITHM_VERSION = 2


def compile_rules(root: Path, *, preview_inventory: dict | None = None) -> dict:
    root = root.resolve()
    rules_path = root / "rules" / "temperate-ecology.json"
    rules_bytes = rules_path.read_bytes()
    rules = json.loads(rules_bytes)
    communities = [r["id"] for r in rules["communities"]]
    if not communities or len(set(communities)) != len(communities):
        raise ValueError("Community IDs must be nonempty and unique")
    if preview_inventory is not None and preview_inventory.get("preview_only") is not True:
        raise ValueError("Preview inventory must be explicitly labelled preview_only")
    inventory = preview_inventory if preview_inventory is not None else compile_inventory(root)
    if inventory["refused"]:
        raise ValueError(f"Invalid publication: {inventory['refused']}")
    traits = {}
    for relative in rules["species_traits"]:
        path = (root / relative).resolve()
        path.relative_to((root / "library").resolve())
        trait_bytes = path.read_bytes()
        body = json.loads(trait_bytes)
        name = body["species"]
        if name in traits or path != root / "library" / name / "ecology.json":
            raise ValueError("Duplicate or misidentified species trait")
        weights = body["community_weights_per_mille"]
        if set(weights) != set(communities) or any(type(v) is not int or not 0 <= v <= 1000 for v in weights.values()):
            raise ValueError(f"Invalid community weights: {name}")
        traits[name] = (body, digest(trait_bytes))
        if body["kind"] != "tree" and body.get("cover_role") not in (
                "sun", "shade", "spring-woodland", "shrub", "wetland", "shade-shrub", "inert"):
            raise ValueError(f"Missing/invalid cover role: {name}")
        if body["kind"] != "tree" and ((body["kind"] == "rock") != (body["cover_role"] == "inert")):
            raise ValueError(f"Rock/canopy role mismatch: {name}")
        for key, maximum in (("crown_opacity_per_mille", 1000), ("trunk_exclusion_mm", 100000)):
            value = body.get(key)
            if type(value) is not int or value < (1 if key == "trunk_exclusion_mm" else 0) or value > maximum:
                raise ValueError(f"Invalid ecological trait {key}: {name}")
    profiles = {}
    missing_traits = []
    for variant in inventory["variants"]:
        name = variant["species"]
        if name not in traits:
            missing_traits.append(variant["id"])
            continue
        trait, trait_hash = traits[name]
        if trait["kind"] != variant["kind"]:
            raise ValueError(f"Trait kind disagrees with asset: {name}")
        if variant.get("growth_form") not in (None, "", "open", "woodland", "edge", "compact", "spreading", "leaning"):
            raise ValueError(f"Unknown growth form: {variant['id']}")
        profile = profiles.setdefault(name, {"species": name, "stable_id": variant["species_stable_id"],
            "kind": variant["kind"], "trait_sha256": trait_hash,
            "cover_role": trait.get("cover_role"),
            "community_weights_per_mille": [trait["community_weights_per_mille"][c] for c in communities],
            "crown_opacity_per_mille": trait["crown_opacity_per_mille"], "variants": []})
        geometry = variant["geometry"]
        if "stand_weights_per_mille" in trait:
            weights=trait['stand_weights_per_mille']
            if not isinstance(weights,list) or len(weights)!=5 or any(type(w) is not int or not 0<=w<=1000 for w in weights):
                raise ValueError(f'Invalid stand weights: {name}')
            profile['stand_weights_per_mille']=weights
        if "density_spacing_mm" in trait:
            spacing, abundance = trait['density_spacing_mm'], trait.get('density_abundance_q10')
            if type(spacing) is not int or not 1 <= spacing <= 1000000 or type(abundance) is not int or not 0 <= abundance <= 1024:
                raise ValueError(f'Invalid ecological density policy: {name}')
            profile.update(density_spacing_mm=spacing, density_abundance_q10=abundance)
        if "ancient_only" in trait:
            if type(trait['ancient_only']) is not bool or trait['kind'] != 'tree':
                raise ValueError(f'Invalid ancient stand policy: {name}')
            profile['ancient_only'] = trait['ancient_only']
        profile["variants"].append({"id": variant["id"], "stable_id": variant["variant_stable_id"],
            "asset_seed": variant["asset_seed"], "bank_file": variant["bank_file"],
            "geometry_sha256": variant["geometry_sha256"], "geometry_md5": variant["geometry_md5"],
            "height_mm": geometry["height_mm"], "bounds_radius_mm": geometry["bounds_radius_mm"],
            "above_anchor_mm": geometry["above_anchor_mm"], "below_anchor_mm": geometry["below_anchor_mm"],
            "trunk_exclusion_mm": trait["trunk_exclusion_mm"], "voxel_pitch_um": geometry["voxel_pitch_um"],
            "size_class": variant["size_class"], "growth_form": variant["growth_form"]})
    profiles = list(profiles.values())
    unrepresented = [c for i, c in enumerate(communities) if not any(p["community_weights_per_mille"][i] for p in profiles)]
    missing_trees = [c for i, c in enumerate(communities)
                     if not any(p["kind"] == "tree" and p["community_weights_per_mille"][i] for p in profiles)]
    return {"schema_version": 1, "algorithm_version": ALGORITHM_VERSION, "preview_only": preview_inventory is not None,
            "revision": rules["revision"], "biomes": rules["biomes"],
            "rules_sha256": digest(rules_bytes), "publication_sha256": inventory["publication_sha256"],
            "fields": rules["fields"], "communities": rules["communities"], "profiles": profiles,
            "readiness": {"published_species": len(profiles), "published_variants": sum(len(p["variants"]) for p in profiles),
                          "unrepresented_communities": unrepresented,
                          "communities_without_trees": missing_trees,
                          "published_variants_missing_traits": missing_traits},
            "activation": "requires_runtime_identity_binding_and_world_validation"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--out", type=Path, default=ROOT / "out" / "ecological-placement" / "placement.json")
    args = parser.parse_args()
    result = compile_rules(args.root)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result["readiness"]))

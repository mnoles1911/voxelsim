"""Install explicit first-pass tree placement traits in authoritative species records.

Community weights are authored game targets, not measured botanical abundance.
Habitat gates remain mandatory; these preferences never override water/slope rules.
"""
from pathlib import Path
import json
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from forge.forest_profiles import PROFILES

COMMUNITIES = {
    "oak-beech": "Eurasian broadleaf woodland",
    "maple-beech": "Eastern North American mixed woodland",
    "coastal-conifer": "Pacific coastal conifer woodland",
    "pine-birch": "Cool transitional woodland",
    "riparian": "Wet-ground and river-edge assemblage",
    "warm-dry": "Warm dry temperate woodland",
    "east-asian": "East Asian regional woodland accents",
    "wet-mild": "Mild humid regional woodland",
}
GROUPS = {
    "oak-beech": "temperate-oak european-beech hornbeam sycamore-maple field-maple small-leaved-lime common-ash field-elm wych-elm birch hawthorn-scrub crab-apple wild-pear rowan sweet-chestnut european-yew temperate-sapling",
    "maple-beech": "american-beech sugar-maple eastern-hemlock black-cherry flowering-dogwood shagbark-hickory tulip-tree honey-locust bur-oak quaking-aspen temperate-sapling",
    "coastal-conifer": "douglas-fir western-hemlock western-red-cedar sitka-spruce bigleaf-maple monterey-cypress hero-sequoia temperate-sapling",
    "pine-birch": "scots-pine norway-spruce birch rowan quaking-aspen tundra-pine temperate-sapling",
    "riparian": "common-alder eastern-cottonwood white-poplar weeping-willow river-broadleaf common-ash temperate-sapling",
    "warm-dry": "cork-oak holm-oak maritime-pine columnar-cypress wild-pear sweet-chestnut temperate-sapling",
    "east-asian": "japanese-maple cherry-blossom weeping-willow temperate-sapling",
    "wet-mild": "tree-fern temperate-sapling",
}


def install(root=ROOT):
    members = {key: set(value.split()) for key, value in GROUPS.items()}
    covered = set().union(*members.values())
    if covered != set(PROFILES):
        raise ValueError(f"Profile coverage differs: {covered ^ set(PROFILES)}")
    results = []
    # Check the full destination set before writing any authoritative record.
    for name in sorted(PROFILES):
        if not (root / "library" / name / "species.json").is_file():
            raise ValueError(f"Missing authoritative species record: {name}")
    for name, profile in sorted(PROFILES.items()):
        body = json.loads((root / "library" / name / "species.json").read_text(encoding="utf-8"))
        base = body["baseline_spec"]
        tree_role = profile.habitat
        weights = {community: (1000 if tree_role in ("canopy", "coastal", "cool", "cool-moist", "cool-dry") else 400)
                   if name in group else 0 for community, group in members.items()}
        # Deliberately uncommon regional accents, not ubiquitous canopy fillers.
        if tree_role in ("montane-hero", "coastal-regional", "warm-regional", "understory-regional", "wet-mild-regional"):
            weights = {k: min(v, 150) for k, v in weights.items()}
        radius = float(base.get("trunk", {}).get("radius_base_m", 0.25))
        record = {"schema_version": 1, "revision": "temperate-ecology-authoring-v1",
                  "species": name, "kind": "tree", "habitat_role": tree_role,
                  "community_weights_per_mille": weights,
                  "trunk_exclusion_mm": max(500, min(5000, round(radius * 1000) + 700)),
                  "crown_opacity_per_mille": min(950, max(450, round(profile.density * 700))),
                  "reference_review": "reference-review.json",
                  "status": "authored_initial_placement_targets_pending_world_visual_review",
                  "notes": ["Preferences do not bypass existing habitat gates.",
                            "Community memberships are game assemblages; regional botanical refinements remain tunable.",
                            "Variant bounds and publication authority come from the inventory, not this trait record."]}
        path = root / "library" / name / "ecology.json"
        if name == 'hero-sequoia':
            record.update(ancient_only=True, density_spacing_mm=48000, density_abundance_q10=819)
            record['notes'].append('Large-specimen game target: restricted to regional ancient stands; replaces legacy density that rounded to zero.')
        if name == 'temperate-sapling':
            record.update(stand_weights_per_mille=[60,1000,120,10,800],
                          density_spacing_mm=6500,density_abundance_q10=819)
            record['notes'].append('Saplings concentrate in young stands and localized thickets; scarce beneath established ancient canopy. Stand order: mixed, young, open, ancient, thicket.')
        path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        results.append(name)
    rules = {"schema_version": 1, "revision": "temperate-ecology-authoring-v1",
             "biomes": ["temperate_forest"], "season": "spring", "simulation": "initial_generation_only",
             "communities": [{"id": k, "label": v} for k, v in COMMUNITIES.items()],
             "fields": {"community_mm": 384000, "stand_mm": 72000, "feature_cell_mm": 160000,
                        "feature_radius_mm": 36000, "ancient_per_mille": 240, "thicket_per_mille": 300},
             "species_traits": [f"library/{n}/ecology.json" for n in results],
             "activation": "not_active_until_compiled_and_runtime_validated"}
    rules_path = root / "rules" / "temperate-ecology.json"
    if rules_path.is_file():
        previous = json.loads(rules_path.read_text(encoding="utf-8"))
        if previous["communities"] != rules["communities"]:
            raise ValueError("Existing community definitions differ; explicitly migrate traits before replacing them")
        rules["species_traits"] = sorted(set(previous["species_traits"]) | set(rules["species_traits"]))
    rules_path.write_text(json.dumps(rules, indent=2) + "\n", encoding="utf-8")
    return {"tree_profiles": len(results), "communities": len(COMMUNITIES)}


if __name__ == "__main__":
    print(json.dumps(install()))

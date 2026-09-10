"""Initial understory placement traits; no asset generation or endorsement.

These are inspectable game targets. Existing water/depth/soil habitat gates
remain separate requirements, and species-level refinements stay editable.
"""
import json
from pathlib import Path
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from forge.understory_profiles import PROFILES

WET=set("""arrowhead blanket-weed bogbean branched-bur-reed broadleaf-cattail bulrush canadian-waterweed
common-cottongrass common-stonewort curled-pondweed flowering-rush fringed-water-lily lesser-pond-sedge
marsh-cinquefoil marsh-marigold needle-spike-rush pickerelweed purple-loosestrife quillwort reed-sweet-grass
rigid-hornwort river-water-crowfoot sedge-tussock soft-rush sphagnum-hummock spiked-water-milfoil sweet-flag
water-chestnut water-earwort water-forget-me-not water-horsetail water-mint water-plantain water-reed water-soldier
water-starwort watercress white-water-lily wild-rice willow-moss yellow-flag-iris yellow-water-lily""".split())
SPRING=set("common-bluebell large-trillium lily-of-the-valley primrose ramsons trout-lily wood-anemone".split())
SHADE=set("""brook-moss-cushion bugle common-dog-violet cyclamen dogs-mercury feather-moss hair-cap-moss
harts-tongue-fern hellebore herb-robert ivy-ground-layer jungle-groundcover jungle-understory-flower lady-fern
male-fern moss-cushion red-campion sword-fern understory-fern wood-horsetail wood-sedge wood-sorrel woolly-fringe-moss""".split())
SHADE_SHRUB=set("bilberry-mat box butchers-broom hazel-coppice holly-understorey mountain-laurel red-huckleberry rhododendron-thicket salal witch-hazel".split())
PACIFIC=set("salal sword-fern red-huckleberry snowberry".split())
EASTERN=set("large-trillium trout-lily mountain-laurel witch-hazel common-milkweed".split())
REFERENCES={
    "common-bluebell":"https://www.woodlandtrust.org.uk/trees-woods-and-wildlife/plants/wild-flowers/bluebell/",
    "salal":"https://research.fs.usda.gov/feis/species-reviews/gausha",
    "sword-fern":"https://research.fs.usda.gov/feis/species-reviews/polmun",
    "water-reed":"https://www.rhs.org.uk/plants/54026/phragmites-australis/details",
}


def install(root=ROOT):
    rules_path=root/'rules'/'temperate-ecology.json'
    rules=json.loads(rules_path.read_text(encoding='utf-8'))
    communities=[c['id'] for c in rules['communities']]
    if not (WET|SPRING|SHADE|SHADE_SHRUB|PACIFIC|EASTERN)<=set(PROFILES):
        raise ValueError('Unknown species in authored placement groups')
    sources={}
    for name in PROFILES:
        sources[name]=json.loads((root/'library'/name/'species.json').read_text(encoding='utf-8'))
    counts={}
    for name,source in sorted(sources.items()):
        kind=source['baseline_spec']['kind']
        if kind not in ('bush','reed','grass','flower'):raise ValueError(f'Unexpected kind: {name}')
        role=('wetland' if name in WET else 'spring-woodland' if name in SPRING else
              'shade-shrub' if name in SHADE_SHRUB else 'shade' if name in SHADE else
              'shrub' if kind=='bush' else 'sun')
        weights={c:500 for c in communities}
        if name in PACIFIC:weights={c:1000 if c=='coastal-conifer' else 0 for c in communities}
        elif name in EASTERN:weights={c:1000 if c=='maple-beech' else 0 for c in communities}
        elif role=='wetland':weights={c:1000 if c=='riparian' else 200 for c in communities}
        elif role=='spring-woodland':weights={c:900 if c=='oak-beech' else 250 for c in communities}
        record={'schema_version':1,'revision':'temperate-ecology-authoring-v1','species':name,'kind':kind,
                'cover_role':role,'community_weights_per_mille':weights,'trunk_exclusion_mm':100,
                'crown_opacity_per_mille':0,'reference_review':'reference-review.json',
                'placement_references':[REFERENCES[name]] if name in REFERENCES else [],
                'status':'authored_initial_placement_targets_pending_world_visual_review',
                'notes':['Instanced non-colliding understory; no voxel-level destruction.',
                         'Water/depth gates are mandatory; wetland role alone does not establish suitability.',
                         'Weights are authored game targets. Broad default communities need regional refinement.',
                         'No seasonal simulation or recoloring.']}
        (root/'library'/name/'ecology.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
        counts[role]=counts.get(role,0)+1
    rules['species_traits']=sorted(set(rules['species_traits'])|{f'library/{n}/ecology.json' for n in sources})
    rules_path.write_text(json.dumps(rules,indent=2)+'\n',encoding='utf-8')
    return {'understory_profiles':len(sources),'roles':counts,'total_trait_references':len(rules['species_traits'])}


if __name__=='__main__':print(json.dumps(install()))

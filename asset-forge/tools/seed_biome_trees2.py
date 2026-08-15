"""Author the thirty trees the first pass left on the biome lists.

WHAT THIS SECOND PASS IS. `tools/seed_biome_trees.py` took the taiga, the
savanna, the desert and the top of the other lists; what was left is the long
tail of the two biggest deciduous tables -- grassland's twenty-six rows and
temperate forest's thirty-six -- plus the beach's mangroves and the two shapes
in rainforest and savanna that nothing shipped covers. Same rules, same
vocabulary, same lattice. Read that file first; everything it says about why a
tree is worth the cost and what 10 cm takes away holds here unchanged and is
not repeated.

TEN CENTIMETRES, AND NOTHING ELSE. `resolution_cm` is "10" on all thirty
because trees join the world's own voxel grid (`forge/kinds.py:29-58`) and
`forge.cli.selftest` refuses a terrain-lattice spec at any other size.

THE MODEL SPLIT IS DIFFERENT THIS TIME, AND THAT IS THE INTERESTING PART.
Twenty-four of the thirty are `colonize`, six are `whorl`, and NONE are
`frond` -- every palm on these lists is already shipped or was authored in the
first pass. Two of the six whorls are broadleaves rather than conifers, which
is the same argument `cecropia` makes: `whorl` is not "the conifer model", it
is the model for a crown whose tiers are a real structure laid down ring by
ring. A flowering dogwood's horizontal layers and a screwpine's repeated fork
are both that, and colonization can only make a crown that is layer-SHAPED.

FIVE SPECIES ARE IDENTIFIED BY THEIR BARK AND THE LATTICE HOLDS NONE OF IT.
Cork oak (a stripped band on the lower trunk only), sweet chestnut (fissures
that spiral), shagbark hickory (plates peeling away at both ends), sycamore
maple (flaking plates) and maritime pine (orange in the fissures). Bark
texture is finer than 10 cm and there is one bark material per spec, so each
of those five is authored on its other half -- bole proportion, crown envelope,
branch architecture -- and each one's `notes` says which half was lost. Between
them and the Scots pine they are the argument for a per-height bark split and a
bark-relief pass, which are two separate generator asks.

WHAT IS DELIBERATELY NOT HERE, WITH THE RULE THAT STOPPED IT:

* **Fallen mossy log** (temperate forest) and **driftwood snag** (beach). Both
  are a trunk LYING DOWN. `trunk.lean_deg` runs 0 to 40 degrees
  (`forge/spec.py`, the trunk block) and `crown.lean_deg` 0 to 45; horizontal
  is 90 and there is no other field that lays an asset over. Both biome files
  already say the answer is `desert-dead` laid down, which is a placement
  rotation and not something a spec can express. Not authored, not faked.
* **Umbrella thorn acacia** (desert). It is the shipped `savanna-acacia`: same
  8 m, same flat-topped umbrella crown on a bare trunk, and the grassland file
  already maps its own "Umbrella acacia" row onto that spec. A second spec
  would be a near-duplicate of one of the most recognisable silhouettes in the
  library. The one real gap is that `savanna-acacia` carries `biomes.desert`
  0.0, so it cannot place there -- a one-field change to a spec this script
  does not own.
* **Giant bamboo clump** and **hazel coppice at full size**, both recorded by
  the previous pass: twenty to forty poles from one base is many pieces and
  one-asset-per-generation forbids it.

    python tools/seed_biome_trees2.py
    python tools/seed_biome_trees2.py --force

SIZES ARE APPROXIMATE. Every height is the figure from the biome file the
species came from, and those files say in their own closing sections that the
numbers are unsourced general-knowledge approximations. Two specs are authored
away from their file's number on purpose and say so in their `notes`. Nothing
here is quoted as measured.
"""
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
import seedspec
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"


def base(**over):
    changes = {
        "kind": "tree",
        "resolution_cm": "10",
        "variation.amount": 1.0,
        "variation.height": 0.18,
        "variation.crown_radius": 0.18,
        "variation.trunk_radius": 0.18,
        "variation.shape": 0.12,
        "variation.proportion": 0.30,
        "variation.lean_deg": 7.0,
        "variation.density": 0.12,
        "variation.droop": 0.25,
        "variation.rotate": True,
    }
    changes.update(over)
    return changes


def t(**kw):
    """`trunk.*`/`crown.*`/... from keywords, with a group prefix per name."""
    groups = {
        "radius_base_m": "trunk", "clear_frac": "trunk", "lean_deg": "trunk",
        "wander": "trunk", "buttress": "trunk",
        "shape": "crown", "radius_m": "crown", "height_frac": "crown",
        "center_frac": "crown", "shell_upper": "crown",
        "shell_lower": "crown", "squash": "crown", "asymmetry": "crown",
        "offset": "crown", "points": "crown", "lean_deg_crown": "crown",
        "model": "growth", "step_m": "growth", "influence_m": "growth",
        "kill_m": "growth", "gravity": "growth", "phototropism": "growth",
        "inertia": "growth", "jitter": "growth", "max_iter": "growth",
        "shade": "growth", "tip_radius_m": "growth", "radius_exp": "growth",
        "enabled": "foliage", "min_order": "foliage",
        "clump_radius_m": "foliage", "density": "foliage", "rough": "foliage",
        "habit": "foliage", "stretch": "foliage", "clustering": "foliage",
        "top_bias": "foliage", "coverage": "foliage", "separation": "foliage",
        "clump_jitter": "foliage", "droop_m": "foliage",
        "squash_f": "foliage", "compensate": "foliage",
    }
    out = {}
    for k, v in kw.items():
        if k.startswith("mat_"):
            out["materials." + k[len("mat_"):]] = v
        elif k.startswith("bio_"):
            out["biomes." + k[len("bio_"):]] = v
        elif k.startswith("place_"):
            out["placement." + k[len("place_"):]] = v
        elif k.startswith("whorl_"):
            out["whorl." + k[len("whorl_"):]] = v
        elif k.startswith("frond_"):
            out["frond." + k[len("frond_"):]] = v
        elif k.startswith("roots_"):
            out["roots." + k[len("roots_"):]] = v
        elif k.startswith("strand_"):
            out["strand." + k[len("strand_"):]] = v
        elif k == "squash_f":
            out["foliage.squash"] = v
        elif k == "lean_deg_crown":
            out["crown.lean_deg"] = v
        else:
            out[f"{groups[k]}.{k}"] = v
    return out


SPECIES = {
    # --- grassland: 28% of the world's land, and a tree in it stands alone ---
    "cork-oak": (
        "12 m - the second dehesa oak: heavy stripped bole, open ragged dome",
        base(name="cork-oak", height_m=12.0,
             notes="THE TWO-TONE TRUNK IS THE SPECIES AND ONE SPEC HAS ONE BARK "
                   "MATERIAL. A stripped cork oak is thick pale corky bark above "
                   "and a bare dark red-brown band below, and `materials.bark` "
                   "colours the whole tree -- the same wall the Scots pine hit "
                   "from the other direction, and the second-strongest argument "
                   "in the library for a per-height bark split. The dark option "
                   "is taken here because the stripped band sits at the bottom "
                   "where the bole is thickest and closest to the eye.\n\n"
                   "What IS authored is the difference from `holm-oak`, which is "
                   "the same genus in the same grass at the same height and "
                   "would otherwise be the same object twice: this one carries "
                   "the heaviest trunk of any 12 m tree here bar the yew, a "
                   "wandering gnarled bole, and a crown that is ragged and open "
                   "where the holm oak's is a closed dark dome (asymmetry 0.42 "
                   "and separation 1.65, against 0.32 and 1.55). Evergreen small "
                   "leaves, so `leaf_needle` for the dark fine-textured mass, "
                   "exactly as the holm oak uses it.",
             **t(radius_base_m=0.60, clear_frac=0.30, lean_deg=6.0, wander=0.35,
                 buttress=0.45,
                 shape="sphere", radius_m=5.0, height_frac=0.62,
                 center_frac=0.68, shell_upper=0.72, shell_lower=0.42,
                 asymmetry=0.42, offset=0.24, points=1700,
                 model="colonize", step_m=0.26, influence_m=2.3, kill_m=0.44,
                 gravity=-0.06, inertia=0.40, jitter=0.14, max_iter=300,
                 shade=0.50, tip_radius_m=0.05, radius_exp=2.35,
                 clump_radius_m=0.55, density=0.70, habit="spiral",
                 stretch=2.2, coverage=0.86, separation=1.65,
                 clump_jitter=0.34, squash_f=0.85,
                 mat_bark="bark", mat_leaf="leaf_needle",
                 bio_grassland=1.0, bio_savanna=0.3,
                 bio_temperate_forest=0.25,
                 place_abundance=0.4, place_spacing_m=13.0, place_cluster=0.4,
                 place_elev_max_m=1000)),
    ),
    "eastern-cottonwood": (
        "25 m - a big open crown that is only ever on a watercourse",
        base(name="eastern-cottonwood", height_m=25.0,
             notes="THE IDENTITY IS WHERE IT IS, AND THAT IS A PLACEMENT "
                   "STATEMENT: `water_max_m` 30 with a high `cluster`, so "
                   "cottonwoods draw a line of big crowns along a creek across "
                   "otherwise empty dry grass. In open country that line is "
                   "visible from further away than the tree is, and a "
                   "cottonwood scattered evenly over a plain would be a lie "
                   "about the biome as well as a waste of the biggest broadleaf "
                   "in the grassland set.\n\n"
                   "The deeply furrowed grey bark is below the lattice. The "
                   "crown carries it instead: broad and OPEN -- separation 1.85 "
                   "and coverage 0.78, well down on the beech's 1.55 and 0.95 "
                   "-- on a heavy bole, so there is daylight through it and a "
                   "readable branch frame inside.",
             **t(radius_base_m=0.75, clear_frac=0.38, lean_deg=3.0, wander=0.16,
                 buttress=0.35,
                 shape="ovoid", radius_m=7.0, height_frac=0.56,
                 center_frac=0.72, shell_upper=0.62, shell_lower=0.30,
                 asymmetry=0.40, offset=0.30, points=1800,
                 model="colonize", step_m=0.32, influence_m=2.8, kill_m=0.48,
                 gravity=-0.08, inertia=0.46, jitter=0.10, max_iter=300,
                 shade=0.45, tip_radius_m=0.05, radius_exp=2.3,
                 clump_radius_m=0.70, density=0.58, habit="spiral",
                 stretch=2.4, coverage=0.78, separation=1.85,
                 clump_jitter=0.36, top_bias=0.40, squash_f=0.85,
                 mat_bark="bark", mat_leaf="leaf_broadleaf",
                 bio_grassland=1.0, bio_temperate_forest=0.45,
                 bio_savanna=0.2,
                 place_abundance=0.5, place_spacing_m=8.0, place_cluster=0.7,
                 place_water_max_m=30, place_slope_max_pct=25)),
    ),
    "white-poplar": (
        "20 m - a pale column in a suckering stand, crown broader than an aspen",
        base(name="white-poplar", height_m=20.0,
             notes="THE TWO-TONE CROWN CANNOT BE SAID AT ALL. A white poplar's "
                   "leaves are dark above and white-felted underneath, so the "
                   "crown flares silver when wind turns it -- that is a "
                   "per-FACE colour on a leaf that does not exist at 10 cm, and "
                   "no material in the palette has two sides. Recorded because "
                   "it is the species' most quoted feature and it is not in "
                   "here.\n\n"
                   "What is here is the pale bole and the stand: `bark_pale` "
                   "with `cluster` 0.9 at 4 m spacing, because a poplar suckers "
                   "and comes in thickets. That makes it the second clonal tree "
                   "in the library and it is deliberately NOT `quaking-aspen`: "
                   "twice the trunk radius, twice the crown, branches "
                   "ascending rather than fine and drooping, and placed against "
                   "water where the aspen is not.",
             **t(radius_base_m=0.30, clear_frac=0.40, lean_deg=4.0, wander=0.14,
                 buttress=0.10,
                 shape="ovoid", radius_m=4.2, height_frac=0.56,
                 center_frac=0.74, shell_upper=0.60, shell_lower=0.32,
                 asymmetry=0.32, offset=0.28, points=1400,
                 model="colonize", step_m=0.26, influence_m=2.2, kill_m=0.40,
                 gravity=0.06, inertia=0.52, jitter=0.10, max_iter=290,
                 tip_radius_m=0.05, radius_exp=2.2,
                 clump_radius_m=0.50, density=0.62, habit="spiral",
                 stretch=2.2, coverage=0.82, separation=1.80,
                 clump_jitter=0.36, squash_f=0.90,
                 mat_bark="bark_pale", mat_leaf="leaf_broadleaf",
                 bio_grassland=1.0, bio_temperate_forest=0.4,
                 place_abundance=0.5, place_spacing_m=4.0, place_cluster=0.9,
                 place_water_max_m=40, place_elev_max_m=1200)),
    ),
    "wild-pear": (
        "8 m - a narrow upright spike of white bloom on a thorny frame",
        base(name="wild-pear", height_m=8.0,
             notes="AUTHORED IN FLOWER, like `cherry-blossom`, because a wild "
                   "pear out of flower is a small brown tree and in flower it is "
                   "the brightest object in a hedge line. `leaf_blossom` at "
                   "coverage 0.95 and separation 1.45 -- the densest crown in "
                   "this pass -- so it reads as one solid white mass rather than "
                   "as foliage that happens to be pale.\n\n"
                   "The shape is the other half, and it is the OPPOSITE of the "
                   "crab apple built beside it: narrow and upright (crown "
                   "radius 1.7 m on 8 m of height, growth lifting at 0.10) where "
                   "the crab apple is low and spreading. Two small rose-family "
                   "trees in one hedge have to differ in silhouette or they are "
                   "one tree twice. The thorns are spur shoots finer than the "
                   "lattice and are not attempted.",
             **t(radius_base_m=0.16, clear_frac=0.28, lean_deg=4.0, wander=0.26,
                 buttress=0.10,
                 shape="ovoid", radius_m=1.7, height_frac=0.68,
                 center_frac=0.68, shell_upper=0.70, shell_lower=0.50,
                 asymmetry=0.30, offset=0.20, points=1000,
                 model="colonize", step_m=0.18, influence_m=1.5, kill_m=0.30,
                 gravity=0.10, inertia=0.45, jitter=0.14, max_iter=260,
                 tip_radius_m=0.05, radius_exp=2.1,
                 clump_radius_m=0.34, density=0.80, habit="rosette",
                 stretch=1.8, coverage=0.95, separation=1.45,
                 clump_jitter=0.28, squash_f=0.85,
                 mat_bark="bark", mat_leaf="leaf_blossom",
                 bio_grassland=1.0, bio_temperate_forest=0.4,
                 bio_savanna=0.15,
                 place_abundance=0.3, place_spacing_m=7.0, place_cluster=0.35,
                 place_elev_max_m=1000)),
    ),
    "crab-apple": (
        "6 m - low, twisted and wider than it is tall: the hedge-line tree",
        base(name="crab-apple", height_m=6.0,
             notes="THE FRUIT IS THE SPECIES AND THERE IS STILL NO FRUIT "
                   "PRIMITIVE. The rowan spec made this ask first and took the "
                   "honest substitution available to it -- an orange crown -- "
                   "because rowan berries hang in heavy clusters that read as "
                   "colour. A crab apple's fruit is scattered single hard balls "
                   "on bare twigs, which does not read as a crown colour, so "
                   "nothing is substituted here: this is the green tree, and the "
                   "fruit pass is now wanted by three species (rowan, crab "
                   "apple, sausage tree).\n\n"
                   "The shape does the work: a crown radius of 2.8 m on a 6 m "
                   "tree, branching from just above the ground, with the highest "
                   "trunk wander in this pass at 0.55. `rosette` habit, which is "
                   "the short-shoot arrangement an apple actually has and the "
                   "one habit that puts leaves back on the inner branches.",
             **t(radius_base_m=0.20, clear_frac=0.14, lean_deg=8.0, wander=0.55,
                 buttress=0.30,
                 shape="sphere", radius_m=2.8, height_frac=0.76,
                 center_frac=0.62, shell_upper=0.62, shell_lower=0.44,
                 asymmetry=0.50, offset=0.30, points=1100,
                 model="colonize", step_m=0.18, influence_m=1.6, kill_m=0.32,
                 gravity=-0.06, inertia=0.34, jitter=0.18, max_iter=260,
                 tip_radius_m=0.05, radius_exp=2.1,
                 clump_radius_m=0.40, density=0.62, habit="rosette",
                 stretch=2.0, coverage=0.82, separation=1.70,
                 clump_jitter=0.40, squash_f=0.85,
                 mat_bark="bark", mat_leaf="leaf_broadleaf",
                 bio_grassland=1.0, bio_temperate_forest=0.5,
                 place_abundance=0.35, place_spacing_m=5.0, place_cluster=0.5,
                 place_elev_max_m=900)),
    ),
    "bur-oak": (
        "15 m - the lone prairie oak: massive horizontal limbs, flat wide dome",
        base(name="bur-oak", height_m=15.0,
             notes="HEAVY HORIZONTAL LIMBS ARE THREE PARAMETERS PULLING "
                   "TOGETHER, and this is the spec that states the recipe: "
                   "`radius_exp` 2.9 keeps a parent branch thick after it "
                   "splits instead of dividing its section evenly, `step_m` "
                   "0.34 makes each segment long so a limb runs rather than "
                   "curls, and `kill_m` 0.55 gives stubby branching at the end "
                   "of it. Compare the shipped `temperate-oak`, which is the "
                   "same genus with an ordinary 2.2-ish falloff: that one has a "
                   "crown of twigs, this one has a crown of BOUGHS, and at "
                   "distance the difference is the whole silhouette.\n\n"
                   "Squashed to 0.75 so the dome is flat and much wider than "
                   "tall, and placed alone -- 18 m spacing at cluster 0.2, the "
                   "loneliest tree in the grassland set. Deeply corky bark is "
                   "below the lattice.",
             **t(radius_base_m=0.72, clear_frac=0.26, lean_deg=4.0, wander=0.24,
                 buttress=0.50,
                 shape="sphere", radius_m=7.5, height_frac=0.60,
                 center_frac=0.62, shell_upper=0.70, shell_lower=0.40,
                 squash=0.75, asymmetry=0.45, offset=0.20, points=1900,
                 model="colonize", step_m=0.34, influence_m=3.0, kill_m=0.55,
                 gravity=-0.02, inertia=0.50, jitter=0.10, max_iter=300,
                 shade=0.45, tip_radius_m=0.07, radius_exp=2.9,
                 clump_radius_m=0.62, density=0.66, habit="spiral",
                 stretch=2.2, coverage=0.84, separation=1.70,
                 clump_jitter=0.34, top_bias=0.50, squash_f=0.85,
                 mat_bark="bark", mat_leaf="leaf_broadleaf",
                 bio_grassland=1.0, bio_savanna=0.35,
                 bio_temperate_forest=0.3,
                 place_abundance=0.35, place_spacing_m=18.0,
                 place_cluster=0.2, place_elev_max_m=1400)),
    ),
    "honey-locust": (
        "18 m - a high vase of very fine foliage you can see straight through",
        base(name="honey-locust", height_m=18.0,
             notes="THE THORNS ARE NOT HERE AND COULD NOT BE. A honey locust's "
                   "trunk carries clusters of long branched spines, and the "
                   "nearest thing this generator has to a spine is a branch -- "
                   "which at 10 cm is one voxel thick and would read as twigs "
                   "sprouting out of the bole, which is a different and wronger "
                   "tree. Left off deliberately rather than approximated.\n\n"
                   "The crown carries the species instead, and it is the "
                   "airiest broadleaf in this pass: clumps of 0.26 m at "
                   "separation 2.0 and coverage 0.72, so the leaf mass is fine "
                   "grain with daylight everywhere between it. That is the same "
                   "small-clump/high-separation setting the `fever-tree` uses "
                   "for a feathery acacia crown, on a vase envelope instead of "
                   "an umbrella.",
             **t(radius_base_m=0.38, clear_frac=0.42, lean_deg=4.0, wander=0.18,
                 buttress=0.20,
                 shape="vase", radius_m=5.0, height_frac=0.52,
                 center_frac=0.76, shell_upper=0.50, shell_lower=0.26,
                 asymmetry=0.38, offset=0.30, points=1600,
                 model="colonize", step_m=0.28, influence_m=2.6, kill_m=0.42,
                 gravity=0.02, inertia=0.46, jitter=0.10, max_iter=300,
                 tip_radius_m=0.05, radius_exp=2.2,
                 clump_radius_m=0.26, density=0.48, habit="distichous",
                 stretch=2.6, coverage=0.72, separation=2.00,
                 clump_jitter=0.40, squash_f=0.70,
                 mat_bark="bark", mat_leaf="leaf_broadleaf",
                 bio_grassland=1.0, bio_temperate_forest=0.3,
                 bio_savanna=0.2,
                 place_abundance=0.4, place_spacing_m=9.0, place_cluster=0.4,
                 place_elev_max_m=1000)),
    ),
    "field-maple": (
        "10 m - a small tight ball of a crown on a hedgerow trunk",
        base(name="field-maple", height_m=10.0,
             notes="THE HEDGEROW TREE, AND IT IS AUTHORED AS A LINE RATHER THAN "
                   "AS A SPECIMEN: 4.5 m spacing at cluster 0.7, which is the "
                   "tightest spacing of any grassland tree here, because a field "
                   "maple is what an unmanaged hedge turns into and it appears "
                   "in runs. Nothing else in the biome fills that role -- the "
                   "oaks stand alone at 12 to 18 m spacing.\n\n"
                   "`opposite` habit, the decussate pairing every maple and ash "
                   "shares and no oak has, on a small dense round crown that is "
                   "most of the tree (`height_frac` 0.72 on a short bole). The "
                   "corky ridged twigs that name it are a centimetre feature and "
                   "are gone.",
             **t(radius_base_m=0.26, clear_frac=0.24, lean_deg=5.0, wander=0.28,
                 buttress=0.25,
                 shape="sphere", radius_m=3.2, height_frac=0.72,
                 center_frac=0.64, shell_upper=0.75, shell_lower=0.50,
                 asymmetry=0.35, offset=0.22, points=1400,
                 model="colonize", step_m=0.20, influence_m=1.9, kill_m=0.34,
                 gravity=-0.08, inertia=0.38, jitter=0.14, max_iter=280,
                 shade=0.50, tip_radius_m=0.05, radius_exp=2.15,
                 clump_radius_m=0.44, density=0.76, habit="opposite",
                 stretch=2.0, coverage=0.92, separation=1.50,
                 clump_jitter=0.30, squash_f=0.85,
                 mat_bark="bark", mat_leaf="leaf_broadleaf",
                 bio_grassland=1.0, bio_temperate_forest=0.7,
                 place_abundance=0.6, place_spacing_m=4.5, place_cluster=0.7,
                 place_elev_max_m=900)),
    ),
    # --- temperate forest: in a closed canopy, bark and bole are all there is -
    "sweet-chestnut": (
        "25 m - the thickest broadleaf bole in the library under a heavy dome",
        base(name="sweet-chestnut", height_m=25.0,
             notes="THE FISSURES SPIRAL AND NOTHING HERE CAN TWIST. Chestnut "
                   "bark runs in deep ridges that wind around the trunk as it "
                   "ages, and the only parameter that could ever say it is "
                   "`trunk.lobes` -- which the `hornbeam` spec records as unsafe "
                   "and left at zero, because grooving the trunk severs the "
                   "joins where limbs and roots attach. Even repaired it would "
                   "cut STRAIGHT grooves; a spiral needs the flutes to rotate as "
                   "they rise, which is a second feature on top of the first.\n\n"
                   "So the bole is authored as mass instead of as texture: 0.95 "
                   "m of base radius, the thickest broadleaf trunk in the "
                   "library at any height, under a big dome carried on long "
                   "shoots (`stretch` 2.8, for leaves that are long and coarse "
                   "rather than round).",
             **t(radius_base_m=0.95, clear_frac=0.34, lean_deg=3.0, wander=0.14,
                 buttress=0.45,
                 shape="ovoid", radius_m=7.0, height_frac=0.58,
                 center_frac=0.70, shell_upper=0.72, shell_lower=0.38,
                 asymmetry=0.36, offset=0.26, points=1900,
                 model="colonize", step_m=0.32, influence_m=2.8, kill_m=0.50,
                 gravity=-0.10, inertia=0.46, jitter=0.10, max_iter=310,
                 shade=0.50, tip_radius_m=0.06, radius_exp=2.5,
                 clump_radius_m=0.80, density=0.70, habit="spiral",
                 stretch=2.8, coverage=0.88, separation=1.65,
                 clump_jitter=0.32, top_bias=0.45, squash_f=0.78,
                 mat_bark="bark", mat_leaf="leaf_broadleaf",
                 bio_temperate_forest=1.0, bio_grassland=0.4,
                 place_abundance=0.5, place_spacing_m=9.0, place_cluster=0.6,
                 place_elev_max_m=1000)),
    ),
    "sycamore-maple": (
        "25 m - a heavy pale-boled dome, the biggest maple here",
        base(name="sycamore-maple", height_m=25.0,
             notes="A BIG PLAIN DOME, AUTHORED ON PURPOSE. The temperate forest "
                   "list is full of species chosen for a peculiarity -- a "
                   "drooping leader, a fluted bole, peeling plates -- and a wood "
                   "made only of those reads as a collection. This is the "
                   "ordinary big canopy tree the others are seen against, and "
                   "the only thing it does unusually is carry `opposite` habit "
                   "at 25 m, which no other tree this size in the library "
                   "does.\n\n"
                   "`bark_pale` for the flaking plate bark, which is the one "
                   "part of the real thing the lattice can gesture at: the "
                   "plates themselves are below 10 cm, but they expose pale wood "
                   "and the overall trunk reads light rather than dark. The "
                   "hanging seed keys have no primitive -- the same fruit ask "
                   "the crab apple files.",
             **t(radius_base_m=0.60, clear_frac=0.32, lean_deg=3.0, wander=0.16,
                 buttress=0.35,
                 shape="sphere", radius_m=7.5, height_frac=0.60,
                 center_frac=0.68, shell_upper=0.76, shell_lower=0.42,
                 asymmetry=0.32, offset=0.26, points=2000,
                 model="colonize", step_m=0.30, influence_m=2.6, kill_m=0.46,
                 gravity=-0.12, inertia=0.44, jitter=0.08, max_iter=310,
                 shade=0.50, tip_radius_m=0.05, radius_exp=2.35,
                 clump_radius_m=0.72, density=0.74, habit="opposite",
                 stretch=2.4, coverage=0.92, separation=1.60,
                 clump_jitter=0.30, top_bias=0.45, squash_f=0.80,
                 mat_bark="bark_pale", mat_leaf="leaf_broadleaf",
                 bio_temperate_forest=1.0, bio_grassland=0.35, bio_taiga=0.2,
                 place_abundance=0.6, place_spacing_m=8.0, place_cluster=0.6,
                 place_elev_max_m=1400)),
    ),
    "sugar-maple": (
        "25 m - the most regular crown in the library, and that is the point",
        base(name="sugar-maple", height_m=25.0,
             **{"variation.lopsided": 0.15},
             notes="ITS FIELD MARK IS A COLOUR AND ITS GEOMETRY IS A SYMMETRY. "
                   "The biome file says the identifying feature is autumn "
                   "colour, which is a palette variant rather than a shape, and "
                   "the honest response is not to author an orange tree and call "
                   "it a species -- `siberian-larch` earns its autumn phase "
                   "because a bare-in-winter CONIFER is a shape nobody confuses, "
                   "and a maple in leaf is not. This is the green tree; the "
                   "autumn fork is one field, `materials.leaf` set to "
                   "`leaf_autumn`, if a seasonal set is ever wanted.\n\n"
                   "What is authored is the regularity, because a sugar maple "
                   "grown in the open really is close to a drawn oval: "
                   "`crown.asymmetry` 0.10 and `crown.offset` 0.06 are the "
                   "lowest in the library, and `variation.lopsided` is dropped "
                   "to 0.15 so seeds stay regular too -- without that last one "
                   "the variation pass would hand back the lopsidedness the "
                   "crown fields just gave up.",
             **t(radius_base_m=0.55, clear_frac=0.36, lean_deg=1.5, wander=0.08,
                 buttress=0.25,
                 shape="ovoid", radius_m=6.0, height_frac=0.62,
                 center_frac=0.70, shell_upper=0.74, shell_lower=0.40,
                 asymmetry=0.10, offset=0.06, points=1900,
                 model="colonize", step_m=0.28, influence_m=2.5, kill_m=0.44,
                 gravity=-0.10, inertia=0.48, jitter=0.06, max_iter=300,
                 shade=0.50, tip_radius_m=0.05, radius_exp=2.3,
                 clump_radius_m=0.66, density=0.74, habit="opposite",
                 stretch=2.3, coverage=0.92, separation=1.60,
                 clump_jitter=0.28, top_bias=0.45, squash_f=0.82,
                 mat_bark="bark", mat_leaf="leaf_broadleaf",
                 bio_temperate_forest=1.0, bio_taiga=0.3, bio_grassland=0.25,
                 place_abundance=0.7, place_spacing_m=8.0, place_cluster=0.7,
                 place_elev_max_m=1500)),
    ),
    "small-leaved-lime": (
        "25 m - a straight bole under a tall narrow dome",
        base(name="small-leaved-lime", height_m=25.0,
             notes="THE BASAL BURR IS A SECOND STRUCTURE AND THE ONE-ASSET RULE "
                   "FORBIDS IT. What identifies an old lime at ground level is "
                   "the mass of sprouts around its foot -- and that is a ring of "
                   "SEPARATE stems, which arrives as extra pieces at "
                   "26-connectivity and fails `tools/buildcheck.py` exactly as "
                   "the bamboo clump and the full-size hazel coppice do. There "
                   "is no half-measure available: `roots` draws ridges leaving "
                   "the trunk, not shoots rising from it. Left out for the same "
                   "documented reason, not overlooked.\n\n"
                   "The rest is straightforward and deliberately vertical -- "
                   "`crown.squash` 1.2 to stretch the dome upward and a very "
                   "clean bole -- so it stands beside the beech and the chestnut "
                   "as the narrow one. `distichous` habit, the two-ranked spray "
                   "a lime shares with the beech and the elm.",
             **t(radius_base_m=0.55, clear_frac=0.42, lean_deg=2.0, wander=0.08,
                 buttress=0.30,
                 shape="ovoid", radius_m=5.5, height_frac=0.60,
                 center_frac=0.74, shell_upper=0.72, shell_lower=0.36,
                 squash=1.20, asymmetry=0.28, offset=0.24, points=1800,
                 model="colonize", step_m=0.28, influence_m=2.4, kill_m=0.44,
                 gravity=-0.10, inertia=0.50, jitter=0.08, max_iter=300,
                 shade=0.50, tip_radius_m=0.05, radius_exp=2.3,
                 clump_radius_m=0.55, density=0.74, habit="distichous",
                 stretch=2.4, coverage=0.90, separation=1.60,
                 clump_jitter=0.30, top_bias=0.40, squash_f=0.80,
                 mat_bark="bark", mat_leaf="leaf_broadleaf",
                 bio_temperate_forest=1.0, bio_grassland=0.3,
                 place_abundance=0.6, place_spacing_m=8.0, place_cluster=0.7,
                 place_elev_max_m=1200)),
    ),
    "wych-elm": (
        "20 m - a broad low fan off a bole that forks near the ground",
        base(name="wych-elm", height_m=20.0,
             notes="THE SHIPPED `field-elm` IS THE SHAPE THIS ONE IS DEFINED "
                   "AGAINST: that is a tall narrow vase on a straight stem, and "
                   "a wych elm forks low and throws a wide flat fan. Same "
                   "envelope name, opposite proportions -- `clear_frac` 0.18 "
                   "against the field elm's high bole, a crown radius of 6.5 m "
                   "at `height_frac` 0.80, and `crown.squash` 0.80 to press the "
                   "fan down. Two elms in one wood that differ only in size "
                   "would be a waste of a spec.\n\n"
                   "`growth.gravity` is positive at 0.06, which is the ash's "
                   "trick and what makes a vase a vase: branches leave the fork "
                   "rising, then the envelope arches them out.",
             **t(radius_base_m=0.55, clear_frac=0.18, lean_deg=4.0, wander=0.22,
                 buttress=0.40,
                 shape="vase", radius_m=6.5, height_frac=0.80,
                 center_frac=0.58, shell_upper=0.65, shell_lower=0.45,
                 squash=0.80, asymmetry=0.40, offset=0.22, points=1700,
                 model="colonize", step_m=0.30, influence_m=2.6, kill_m=0.46,
                 gravity=0.06, inertia=0.46, jitter=0.10, max_iter=300,
                 shade=0.45, tip_radius_m=0.05, radius_exp=2.4,
                 clump_radius_m=0.60, density=0.68, habit="distichous",
                 stretch=2.4, coverage=0.86, separation=1.70,
                 clump_jitter=0.34, squash_f=0.82,
                 mat_bark="bark", mat_leaf="leaf_broadleaf",
                 bio_temperate_forest=1.0, bio_grassland=0.3, bio_taiga=0.15,
                 place_abundance=0.5, place_spacing_m=8.0, place_cluster=0.5,
                 place_elev_max_m=1100)),
    ),
    "sitka-spruce": (
        "45 m - a rigid spire: the stiffest conifer in the library",
        base(name="sitka-spruce", height_m=45.0,
             notes="RIGIDITY IS THE WHOLE DIFFERENCE FROM `norway-spruce`, and "
                   "it is one number: `whorl.droop` 0.45 here against 1.05 "
                   "there. A Norway spruce's branches hang; a Sitka's stand out "
                   "stiff and slightly up, and at forest distance that is what "
                   "separates two dark green cones. `growth.inertia` 0.66 and a "
                   "low `whorl.irregular` carry the same idea into the "
                   "branching, and the envelope is `spire` rather than `cone`, "
                   "so the widest point sits a sixth of the way up instead of a "
                   "third.\n\n"
                   "THE BLUE-GREY CAST IS NOT AVAILABLE. There is one needle "
                   "material and it is green; a Sitka reads glaucous from below "
                   "because of the white banding on the underside of the needle, "
                   "which is a leaf-scale feature twice over -- too fine for the "
                   "lattice and too specific for the palette. At 45 m it is the "
                   "tallest tree in this pass and the third tallest in the "
                   "library.",
             **t(radius_base_m=0.85, clear_frac=0.30, lean_deg=1.0, wander=0.04,
                 buttress=0.30,
                 shape="spire", radius_m=4.4, height_frac=0.72,
                 center_frac=0.62, shell_upper=0.70, shell_lower=0.48,
                 asymmetry=0.16, offset=0.08, points=1700,
                 model="whorl", step_m=0.34, influence_m=3.0, kill_m=0.60,
                 gravity=-0.18, inertia=0.66, jitter=0.05, max_iter=340,
                 tip_radius_m=0.06, radius_exp=2.5,
                 whorl_count=24, whorl_branches=6, whorl_droop=0.45,
                 whorl_rise=0.28, whorl_sub=2, whorl_irregular=0.18,
                 whorl_leader=0.05,
                 clump_radius_m=0.38, density=0.74, habit="radial",
                 stretch=4.0, coverage=0.95, separation=1.50,
                 clump_jitter=0.28, squash_f=0.60, droop_m=0.10,
                 mat_bark="bark", mat_leaf="leaf_needle",
                 bio_temperate_forest=1.0, bio_taiga=0.5,
                 place_abundance=0.7, place_spacing_m=7.0, place_cluster=0.85,
                 place_water_max_m=120, place_slope_max_pct=55,
                 place_elev_max_m=900)),
    ),
    "western-red-cedar": (
        "45 m - a buttressed base under long drooping sprays",
        base(name="western-red-cedar", height_m=45.0,
             notes="AUTHORED AT 45 m AGAINST THE FILE'S 50, for grid cost rather "
                   "than botany -- the same trade `kapok` records against its "
                   "own list, and the same one `douglas-fir` took silently at 40 "
                   "against 50. Recorded so the number is a decision.\n\n"
                   "`pendulous` FOLIAGE ON A CONIFER, which nothing else here "
                   "does: a red cedar's sprays hang in flat drooping fans, so "
                   "the habit that was written for a willow is the right one, "
                   "with `whorl.droop` at 1.35 and `growth.gravity` -0.42 under "
                   "it. The buttressed fluted base is `trunk.buttress` 0.85 plus "
                   "six real root ridges -- buttress alone only fattens a "
                   "cylinder, which is what the alder spec found. The stringy "
                   "shredding red-brown bark is a surface below the lattice and "
                   "is gone.",
             **t(radius_base_m=1.05, clear_frac=0.20, lean_deg=1.5, wander=0.06,
                 buttress=0.85,
                 shape="cone", radius_m=5.0, height_frac=0.85,
                 center_frac=0.52, shell_upper=0.72, shell_lower=0.55,
                 asymmetry=0.22, offset=0.12, points=1800,
                 model="whorl", step_m=0.32, influence_m=2.8, kill_m=0.55,
                 gravity=-0.42, inertia=0.55, jitter=0.08, max_iter=340,
                 tip_radius_m=0.05, radius_exp=2.5,
                 whorl_count=22, whorl_branches=6, whorl_droop=1.35,
                 whorl_rise=0.16, whorl_sub=3, whorl_irregular=0.28,
                 whorl_leader=0.03,
                 roots_count=6, roots_length_m=3.0, roots_rise=0.30,
                 roots_thickness=0.50, roots_irregular=0.35,
                 clump_radius_m=0.34, density=0.76, habit="pendulous",
                 stretch=4.4, coverage=0.94, separation=1.45,
                 clump_jitter=0.30, squash_f=0.55, droop_m=0.25,
                 mat_bark="bark", mat_leaf="leaf_needle",
                 bio_temperate_forest=1.0, bio_taiga=0.3,
                 place_abundance=0.6, place_spacing_m=8.0, place_cluster=0.8,
                 place_water_max_m=100, place_slope_max_pct=50,
                 place_elev_max_m=1000)),
    ),
    "bigleaf-maple": (
        "25 m - heavy limbs loaded to the inside: the moss-carrier",
        base(name="bigleaf-maple", height_m=25.0,
             notes="THE EPIPHYTE LOAD IS THE SPECIES AND THERE IS NO EPIPHYTE "
                   "PASS, so it is approached from the only direction available: "
                   "put the mass ON THE LIMBS instead of at the crown surface. "
                   "Three settings do it together -- `foliage.min_order` 1, so "
                   "clumps start on the big branches rather than waiting for "
                   "twigs; `foliage.top_bias` 0.0, the only zero in the library, "
                   "so nothing is moved to the top; and `growth.shade` 0.25, "
                   "well under the usual 0.4-0.55, so branches keep growing into "
                   "their own interior and there is something in there to load. "
                   "`rosette` habit finishes it, being the one arrangement that "
                   "puts foliage back on older wood.\n\n"
                   "It is drawn in leaf green because there is no moss material "
                   "and a maple's own leaves are the biggest of any maple. A "
                   "dedicated epiphyte material -- moss on the upper side of a "
                   "limb -- is the real fix and would serve the fallen log and "
                   "the rainforest set too.",
             **t(radius_base_m=0.60, clear_frac=0.22, lean_deg=5.0, wander=0.30,
                 buttress=0.40,
                 shape="sphere", radius_m=7.0, height_frac=0.74,
                 center_frac=0.62, shell_upper=0.70, shell_lower=0.62,
                 squash=0.90, asymmetry=0.45, offset=0.26, points=1900,
                 model="colonize", step_m=0.32, influence_m=2.6, kill_m=0.50,
                 gravity=-0.06, inertia=0.42, jitter=0.14, max_iter=300,
                 shade=0.25, tip_radius_m=0.06, radius_exp=2.6,
                 min_order=1,
                 clump_radius_m=0.85, density=0.72, habit="rosette",
                 stretch=2.0, coverage=0.90, separation=1.55,
                 clump_jitter=0.36, top_bias=0.0, squash_f=0.95,
                 mat_bark="bark", mat_leaf="leaf_broadleaf",
                 bio_temperate_forest=1.0, bio_rainforest=0.2,
                 place_abundance=0.5, place_spacing_m=8.0, place_cluster=0.7,
                 place_water_max_m=80, place_slope_max_pct=50,
                 place_elev_max_m=900)),
    ),
    "tulip-tree": (
        "35 m - the straightest bole in the library, crown held right at the top",
        base(name="tulip-tree", height_m=35.0,
             notes="STRAIGHTNESS IS THE ONE THING THE LATTICE KEEPS. The leaf "
                   "silhouette that names this tree -- squared off at the tip, "
                   "as if cut -- is a leaf-scale mark and at 10 cm there are no "
                   "leaves, only clumps. What survives is the stem: "
                   "`trunk.wander` 0.05 and `trunk.lean_deg` 1.5, the least "
                   "wandering broadleaf here, with `clear_frac` 0.55 so more "
                   "than half the tree is clean bole. Against the crooked crowns "
                   "either side of it in this pass that reads as a distinct "
                   "tree, which is the test.\n\n"
                   "The crown is small for the height and held right at the top "
                   "(`height_frac` 0.44, `center_frac` 0.80), which is what a "
                   "forest-grown tulip tree looks like and also what makes it "
                   "cheap at 35 m.",
             **t(radius_base_m=0.65, clear_frac=0.55, lean_deg=1.5, wander=0.05,
                 buttress=0.25,
                 shape="ovoid", radius_m=5.5, height_frac=0.44,
                 center_frac=0.80, shell_upper=0.65, shell_lower=0.32,
                 asymmetry=0.26, offset=0.20, points=1700,
                 model="colonize", step_m=0.32, influence_m=2.7, kill_m=0.46,
                 gravity=0.02, inertia=0.55, jitter=0.06, max_iter=310,
                 shade=0.50, tip_radius_m=0.05, radius_exp=2.3,
                 clump_radius_m=0.62, density=0.70, habit="spiral",
                 stretch=2.2, coverage=0.88, separation=1.65,
                 clump_jitter=0.30, top_bias=0.50, squash_f=0.85,
                 mat_bark="bark", mat_leaf="leaf_broadleaf",
                 bio_temperate_forest=1.0, bio_grassland=0.2,
                 place_abundance=0.5, place_spacing_m=9.0, place_cluster=0.6,
                 place_elev_max_m=1000)),
    ),
    "american-beech": (
        "25 m - a beech that keeps its lower crown, in dense stands",
        base(name="american-beech", height_m=25.0,
             notes="THE WINTER LEAVES ARE A SECOND LEAF COLOUR ON ONE TREE AND "
                   "THE SPEC HAS ONE. An American beech holds dead tan leaves on "
                   "its lower branches through the winter while the top is bare, "
                   "and that is the same shape of ask as the Scots pine's "
                   "two-tone trunk: a material that varies with height. Both are "
                   "one feature and it would serve four or five species. Noted "
                   "here rather than approximated with a whole-crown autumn "
                   "material, which would just be a wrong-coloured summer "
                   "tree.\n\n"
                   "What IS authored is the difference from `european-beech`, "
                   "five metres taller and beside it in the same wood: this one "
                   "branches lower (`clear_frac` 0.34 against 0.45), carries a "
                   "much heavier lower crown (`shell_lower` 0.48 against 0.35, "
                   "`top_bias` 0.25 against 0.45) and stands in tighter thickets "
                   "-- 7 m spacing at cluster 0.9, because it suckers. The "
                   "sucker thicket itself is separate stems and the one-asset "
                   "rule forbids it, as with the lime.",
             **t(radius_base_m=0.60, clear_frac=0.34, lean_deg=2.0, wander=0.08,
                 buttress=0.28,
                 shape="ovoid", radius_m=6.0, height_frac=0.66,
                 center_frac=0.66, shell_upper=0.72, shell_lower=0.48,
                 asymmetry=0.30, offset=0.24, points=1800,
                 model="colonize", step_m=0.30, influence_m=2.6, kill_m=0.48,
                 gravity=-0.14, inertia=0.46, jitter=0.08, max_iter=300,
                 shade=0.50, tip_radius_m=0.05, radius_exp=2.3,
                 clump_radius_m=0.75, density=0.70, habit="distichous",
                 stretch=2.6, coverage=0.92, separation=1.60,
                 clump_jitter=0.30, top_bias=0.25, squash_f=0.75,
                 mat_bark="bark_pale", mat_leaf="leaf_broadleaf",
                 bio_temperate_forest=1.0, bio_taiga=0.2,
                 place_abundance=0.8, place_spacing_m=7.0, place_cluster=0.9,
                 place_slope_max_pct=50, place_elev_max_m=1200)),
    ),
    "eastern-hemlock": (
        "30 m - clothed to the ground, dark, and always on a stream slope",
        base(name="eastern-hemlock", height_m=30.0,
             notes="TWO HEMLOCKS, TOLD APART BY WHERE THEY START AND WHERE THEY "
                   "STAND. `western-hemlock` is authored around its nodding "
                   "leader and carries a bole clear for a fifth of its height; "
                   "this one is five metres shorter, has `clear_frac` 0.16 so "
                   "the branches sweep the ground, runs the deepest foliage "
                   "coverage of any conifer here (0.96 at separation 1.40), and "
                   "is placed against water on steep ground -- `water_max_m` 60 "
                   "with a 60% slope ceiling. A ravine full of these is dark at "
                   "noon, which is the thing the biome file asks for.\n\n"
                   "`distichous` habit, because a hemlock's needles lie flat "
                   "into one plane and that is what makes the spray look like a "
                   "spray rather than a bottlebrush.",
             **t(radius_base_m=0.55, clear_frac=0.16, lean_deg=2.0, wander=0.12,
                 buttress=0.20,
                 shape="cone", radius_m=3.8, height_frac=0.90,
                 center_frac=0.52, shell_upper=0.74, shell_lower=0.58,
                 asymmetry=0.22, offset=0.12, points=1700,
                 model="whorl", step_m=0.26, influence_m=2.5, kill_m=0.50,
                 gravity=-0.36, inertia=0.55, jitter=0.08, max_iter=330,
                 tip_radius_m=0.05, radius_exp=2.45,
                 whorl_count=22, whorl_branches=6, whorl_droop=1.25,
                 whorl_rise=0.16, whorl_sub=3, whorl_irregular=0.28,
                 whorl_leader=0.04,
                 clump_radius_m=0.28, density=0.78, habit="distichous",
                 stretch=4.2, coverage=0.96, separation=1.40,
                 clump_jitter=0.30, squash_f=0.55, droop_m=0.20,
                 mat_bark="bark", mat_leaf="leaf_needle",
                 bio_temperate_forest=1.0, bio_taiga=0.35,
                 place_abundance=0.6, place_spacing_m=5.5, place_cluster=0.9,
                 place_water_max_m=60, place_slope_max_pct=60,
                 place_elev_max_m=1400)),
    ),
    "shagbark-hickory": (
        "25 m - a narrow stiff column of a crown on a straight grey bole",
        base(name="shagbark-hickory", height_m=25.0,
             notes="THE BARK IS THE SPECIES AND THIS IS THE CLEAREST CASE IN THE "
                   "LIBRARY FOR A BARK-RELIEF PASS. Long plates curling away "
                   "from the trunk at both ends is a SILHOUETTE feature, not a "
                   "texture one -- it breaks the outline of the bole, which is "
                   "the sort of thing a voxel lattice normally keeps. It fails "
                   "here on thickness rather than length: a plate stands off the "
                   "trunk by far less than one 10 cm voxel, so there is nothing "
                   "to draw. `trunk.lobes` cuts INWARD and is unsafe besides "
                   "(see `hornbeam`); this wants material added outward.\n\n"
                   "Authored on proportion instead, and it is genuinely "
                   "distinctive: a `column` crown only 3.6 m in radius on a 25 m "
                   "tree -- the narrowest crown-to-height of any broadleaf here "
                   "-- with stiff ascending limbs (`gravity` +0.08) and an open "
                   "coverage of 0.80.",
             **t(radius_base_m=0.50, clear_frac=0.44, lean_deg=2.0, wander=0.10,
                 buttress=0.20,
                 shape="column", radius_m=3.6, height_frac=0.55,
                 center_frac=0.74, shell_upper=0.60, shell_lower=0.35,
                 asymmetry=0.32, offset=0.24, points=1500,
                 model="colonize", step_m=0.30, influence_m=2.5, kill_m=0.44,
                 gravity=0.08, inertia=0.52, jitter=0.08, max_iter=300,
                 shade=0.45, tip_radius_m=0.05, radius_exp=2.4,
                 clump_radius_m=0.60, density=0.62, habit="spiral",
                 stretch=2.6, coverage=0.80, separation=1.85,
                 clump_jitter=0.34, squash_f=0.85,
                 mat_bark="bark_pale", mat_leaf="leaf_broadleaf",
                 bio_temperate_forest=1.0, bio_grassland=0.3,
                 place_abundance=0.5, place_spacing_m=8.0, place_cluster=0.5,
                 place_elev_max_m=1200)),
    ),
    "black-cherry": (
        "20 m - a narrow dark tree that grows in the gaps",
        base(name="black-cherry", height_m=20.0,
             notes="A NARROW CROWN IS A COMPETITIVE HABIT AND IT IS AUTHORED AS "
                   "ONE: 3.6 m of crown radius on 20 m of height, which is "
                   "half the beech's spread, because this is a tree that fills "
                   "canopy gaps rather than holding a place in the roof. Dark "
                   "bark, dark crown, and enough separation (1.65) that the "
                   "narrow frame stays visible through it.\n\n"
                   "THE FLOWER RACEMES ARE NOT ATTEMPTED. They hang in long "
                   "drooping strings, and the only primitive that hangs is "
                   "`strand`, which draws WOOD carrying the crown's own leaf "
                   "material -- so a strand set would come out as either bare "
                   "broken twigs or a small weeping willow. A hanging-flower "
                   "pass is a real ask and the `sausage-tree` in this same "
                   "script makes the harder version of it.",
             **t(radius_base_m=0.42, clear_frac=0.40, lean_deg=3.0, wander=0.14,
                 buttress=0.18,
                 shape="ovoid", radius_m=3.6, height_frac=0.58,
                 center_frac=0.74, shell_upper=0.62, shell_lower=0.35,
                 asymmetry=0.34, offset=0.28, points=1400,
                 model="colonize", step_m=0.26, influence_m=2.2, kill_m=0.40,
                 gravity=-0.14, inertia=0.46, jitter=0.10, max_iter=290,
                 shade=0.50, tip_radius_m=0.05, radius_exp=2.25,
                 clump_radius_m=0.46, density=0.68, habit="spiral",
                 stretch=2.4, coverage=0.86, separation=1.65,
                 clump_jitter=0.32, squash_f=0.85,
                 mat_bark="bark", mat_leaf="leaf_broadleaf",
                 bio_temperate_forest=1.0, bio_grassland=0.35,
                 place_abundance=0.5, place_spacing_m=6.0, place_cluster=0.5,
                 place_elev_max_m=1200)),
    ),
    "flowering-dogwood": (
        "8 m - flat horizontal tiers of white under the canopy",
        base(name="flowering-dogwood", height_m=8.0,
             notes="A BROADLEAF ON `whorl`, AND THE REASON IS THE SAME ONE THE "
                   "MODEL EXISTS FOR. A dogwood's branches leave the stem in "
                   "distinct horizontal layers with gaps between them; that is a "
                   "real tiered structure, and colonization can only produce a "
                   "crown that is tier-SHAPED. Six rings of four at "
                   "`whorl.rise` 0.02 -- as close to horizontal as the parameter "
                   "goes -- with `whorl.sub_angle` 60 so each layer spreads "
                   "flat, and `foliage.squash` 0.45 to press the clumps into "
                   "plates rather than balls. `cecropia` made this argument "
                   "first at rainforest scale; this is it at understorey "
                   "scale.\n\n"
                   "Drawn in `leaf_blossom` because the white 'flowers' are "
                   "large bracts held flat on top of those layers, which is one "
                   "of the few cases where a whole-crown flower material is "
                   "literally right rather than a substitution.",
             **t(radius_base_m=0.16, clear_frac=0.22, lean_deg=6.0, wander=0.20,
                 buttress=0.12,
                 shape="umbrella", radius_m=2.6, height_frac=0.70,
                 center_frac=0.62, shell_upper=0.55, shell_lower=0.40,
                 asymmetry=0.35, offset=0.24, points=900,
                 model="whorl", step_m=0.20, influence_m=1.8, kill_m=0.40,
                 gravity=-0.05, inertia=0.60, jitter=0.08, max_iter=260,
                 tip_radius_m=0.05, radius_exp=2.3,
                 whorl_count=6, whorl_branches=4, whorl_droop=0.25,
                 whorl_rise=0.02, whorl_sub=2, whorl_sub_angle=60.0,
                 whorl_irregular=0.40, whorl_leader=0.02,
                 clump_radius_m=0.40, density=0.66, habit="distichous",
                 stretch=2.2, coverage=0.85, separation=1.70,
                 clump_jitter=0.32, squash_f=0.45,
                 mat_bark="bark", mat_leaf="leaf_blossom",
                 bio_temperate_forest=1.0, bio_grassland=0.2,
                 place_abundance=0.5, place_spacing_m=5.0, place_cluster=0.5,
                 place_elev_max_m=900)),
    ),
    "japanese-maple": (
        "5 m - a low red layered fan, and the twigs are at the lattice floor",
        base(name="japanese-maple", height_m=5.0,
             notes="THE FINE TWIG STRUCTURE IS EXACTLY AT THE FLOOR AND THAT IS "
                   "WORTH SAYING PLAINLY. What makes this tree is a dense fan of "
                   "very thin twigs held in layers, and at 10 cm one twig is ONE "
                   "VOXEL -- the thinnest thing the lattice can draw. So the "
                   "twig frame is present but it cannot be fine, and no setting "
                   "recovers that: `growth.tip_radius_m` is already at the "
                   "practical floor. `step_m` 0.14 and `kill_m` 0.22 are the "
                   "shortest segments and longest thin twigs in this pass, which "
                   "gets the density of the fan even though each strand is as "
                   "coarse as everything else's.\n\n"
                   "`leaf_autumn` for the deep red, which is the nearest the "
                   "palette reaches and is a fair likeness of a red-leaved "
                   "cultivar in summer. `opposite` habit and a wide low "
                   "umbrella, so it is the smallest and broadest tree in the "
                   "temperate set.",
             **t(radius_base_m=0.14, clear_frac=0.12, lean_deg=8.0, wander=0.40,
                 buttress=0.25,
                 shape="umbrella", radius_m=2.4, height_frac=0.80,
                 center_frac=0.60, shell_upper=0.60, shell_lower=0.45,
                 asymmetry=0.45, offset=0.28, points=1100,
                 model="colonize", step_m=0.14, influence_m=1.2, kill_m=0.22,
                 gravity=-0.10, inertia=0.34, jitter=0.16, max_iter=260,
                 tip_radius_m=0.05, radius_exp=2.1,
                 clump_radius_m=0.28, density=0.62, habit="opposite",
                 stretch=2.0, coverage=0.84, separation=1.60,
                 clump_jitter=0.34, squash_f=0.60,
                 mat_bark="bark_pale", mat_leaf="leaf_autumn",
                 bio_temperate_forest=1.0,
                 place_abundance=0.2, place_spacing_m=6.0, place_cluster=0.3,
                 place_elev_max_m=1200)),
    ),
    # --- beach: the band that wraps every shore, plus the tidal edge ---------
    "red-mangrove": (
        "7 m - a low crown standing on a cage of arching stilt roots",
        base(name="red-mangrove", height_m=7.0,
             notes="ONE SPEC, TWO BIOMES, BECAUSE IT IS ONE SPECIES. The beach "
                   "list and the rainforest list both carry a red mangrove, at 6 "
                   "m and 8 m respectively; this is authored at 7 with a weight "
                   "in each, rather than shipping the same tree twice at two "
                   "sizes. The rainforest file's own range note is worth "
                   "keeping: ground within about three metres of sea level is "
                   "decided as beach before the climate table is reached, so the "
                   "rainforest weight may only ever fire on the landward side of "
                   "an estuary.\n\n"
                   "THE ROOTS ARE HALF THE SILHOUETTE AND THEY ARE THE HEAVIEST "
                   "IN THE LIBRARY: `roots.count` 14 of a maximum 16, at "
                   "`roots.rise` 1.30 of 1.50, against the kapok's 8 at 0.25 and "
                   "the alder's 7 at 0.55. That is the beach file's own "
                   "instruction -- the `roots` group pushed much further than "
                   "any shipped spec uses it -- and what it gives is a base of "
                   "high arches meeting the ground well outside the trunk, with "
                   "the crown sitting on top of them. The trunk itself is thin "
                   "for the height because the arches carry the tree.",
             **t(radius_base_m=0.28, clear_frac=0.35, lean_deg=6.0, wander=0.25,
                 buttress=0.20,
                 shape="ovoid", radius_m=3.2, height_frac=0.62,
                 center_frac=0.72, shell_upper=0.70, shell_lower=0.45,
                 asymmetry=0.40, offset=0.24, points=1400,
                 model="colonize", step_m=0.22, influence_m=1.9, kill_m=0.36,
                 gravity=-0.06, inertia=0.40, jitter=0.14, max_iter=280,
                 tip_radius_m=0.05, radius_exp=2.2,
                 roots_count=14, roots_length_m=3.0, roots_rise=1.30,
                 roots_thickness=0.60, roots_irregular=0.45,
                 clump_radius_m=0.52, density=0.78, habit="spiral",
                 stretch=2.0, coverage=0.92, separation=1.50,
                 clump_jitter=0.30, squash_f=0.85,
                 mat_bark="bark", mat_leaf="leaf_jungle",
                 bio_beach=1.0, bio_rainforest=0.8,
                 place_abundance=0.6, place_spacing_m=3.5, place_cluster=0.95,
                 place_water_max_m=10, place_elev_max_m=8,
                 place_slope_max_pct=15)),
    ),
    "black-mangrove": (
        "8 m - an ordinary crown on cable roots; the spikes are NOT here",
        base(name="black-mangrove", height_m=8.0,
             notes="THE IDENTIFYING FEATURE IS ABSENT AND THE BIOME FILE ALREADY "
                   "SAID IT WOULD BE. A black mangrove is recognised by a field "
                   "of finger-thick vertical pneumatophores over the mud around "
                   "it; the beach file's own warning marks the row and says a "
                   "spike of that size is a fraction of a 10 cm voxel. Two "
                   "further reasons it is not attempted here even stylised: a "
                   "field of spikes spread over tens of metres of mud is GROUND "
                   "COVER rather than part of this tree, and spikes standing "
                   "clear of the trunk arrive as separate pieces, which "
                   "one-asset-per-generation forbids and `tools/buildcheck.py` "
                   "would reject.\n\n"
                   "What IS authored is the part that belongs to the tree: five "
                   "low cable roots (`roots.rise` 0.10, so they run along the "
                   "mud rather than arching like the red mangrove's stilts) "
                   "under a dense ordinary dome. Beside the red mangrove, the "
                   "pair reads as two mangroves -- one standing on arches, one "
                   "sitting flat on the mud -- which is the honest half of the "
                   "difference.",
             **t(radius_base_m=0.34, clear_frac=0.30, lean_deg=5.0, wander=0.30,
                 buttress=0.45,
                 shape="sphere", radius_m=3.4, height_frac=0.66,
                 center_frac=0.68, shell_upper=0.72, shell_lower=0.48,
                 asymmetry=0.38, offset=0.22, points=1500,
                 model="colonize", step_m=0.24, influence_m=2.0, kill_m=0.38,
                 gravity=-0.06, inertia=0.40, jitter=0.14, max_iter=280,
                 tip_radius_m=0.05, radius_exp=2.2,
                 roots_count=5, roots_length_m=1.6, roots_rise=0.10,
                 roots_thickness=0.45, roots_irregular=0.40,
                 clump_radius_m=0.50, density=0.76, habit="spiral",
                 stretch=2.0, coverage=0.90, separation=1.55,
                 clump_jitter=0.32, squash_f=0.85,
                 mat_bark="bark", mat_leaf="leaf_jungle",
                 bio_beach=1.0, bio_rainforest=0.4,
                 place_abundance=0.5, place_spacing_m=4.0, place_cluster=0.9,
                 place_water_max_m=15, place_elev_max_m=6,
                 place_slope_max_pct=12)),
    ),
    "screwpine": (
        "6 m - a palm assembled wrong: a forking stem on stilts, tuft per tip",
        base(name="screwpine", height_m=6.0,
             notes="`whorl` FOR A THING THAT LOOKS LIKE A PALM, for the same "
                   "reason the `doum-palm` uses it: `frond` builds an "
                   "UNBRANCHED stem with one crown, by definition, and a "
                   "screwpine forks. Four rings of two long branches leaving at "
                   "a strong lift, one sub-branch each, and a big tuft on every "
                   "tip -- so the plant is a few thick arms each ending in a "
                   "rosette, which is what it actually is.\n\n"
                   "The stilts are `roots` at 10 of 16 with a high arch, second "
                   "only to the red mangrove in this pass. THE SCREW ITSELF IS "
                   "GONE: the name comes from the spiral in which the strap "
                   "leaves are set around each tip, and a leaf spiral is a "
                   "centimetre-scale arrangement of things that are not drawn "
                   "individually at 10 cm. What is left is the assembly, and the "
                   "assembly is odd enough to be recognisable.",
             **t(radius_base_m=0.20, clear_frac=0.30, lean_deg=8.0, wander=0.25,
                 buttress=0.15,
                 shape="ovoid", radius_m=2.4, height_frac=0.68,
                 center_frac=0.66, shell_upper=0.55, shell_lower=0.40,
                 asymmetry=0.40, offset=0.20, points=500,
                 model="whorl", step_m=0.26, influence_m=2.4, kill_m=0.60,
                 gravity=0.10, inertia=0.66, jitter=0.08, max_iter=260,
                 tip_radius_m=0.07, radius_exp=2.7,
                 whorl_count=4, whorl_branches=2, whorl_droop=0.25,
                 whorl_rise=0.45, whorl_sub=1, whorl_sub_angle=50.0,
                 whorl_irregular=0.45, whorl_leader=0.08,
                 roots_count=10, roots_length_m=2.2, roots_rise=1.20,
                 roots_thickness=0.55, roots_irregular=0.40,
                 clump_radius_m=0.70, density=0.80, habit="tuft",
                 stretch=1.6, coverage=1.0, separation=1.50,
                 clump_jitter=0.22, squash_f=0.70,
                 mat_bark="bark_pale", mat_leaf="leaf_jungle",
                 bio_beach=1.0, bio_rainforest=0.3,
                 place_abundance=0.4, place_spacing_m=4.0, place_cluster=0.8,
                 place_water_max_m=20, place_elev_max_m=30)),
    ),
    "maritime-pine": (
        "20 m - a long bare stem with a rough tiered crown in the top quarter",
        base(name="maritime-pine", height_m=20.0,
             notes="THE THIRD PINE, AND IT HAS TO DIFFER FROM BOTH THE OTHERS AT "
                   "DISTANCE. `stone-pine` is a flat plate on a bare stem "
                   "(`height_frac` 0.20, umbrella); `scots-pine` is a high open "
                   "irregular flat top; this one keeps the bare stem -- "
                   "`clear_frac` 0.72 -- but carries a ROUNDED tiered crown in "
                   "the top quarter, on seven wide-spaced whorls. A pine coast "
                   "with three pines on it reads as a place; with one it reads "
                   "as a texture.\n\n"
                   "THE ORANGE IN THE FISSURES IS NOT AVAILABLE IN EITHER "
                   "DIRECTION. `scots-pine` chose `bark_pale` and got the "
                   "brightness right and the hue wrong; this one takes the dark "
                   "`bark`, which is right for the deeply fissured lower stem "
                   "and wrong further up. That is not a fix, but it does mean "
                   "the two pines differ in trunk value as well as in crown, "
                   "and the underlying ask -- bark colour that varies with "
                   "height -- is the same one three specs in this pass make.",
             **t(radius_base_m=0.48, clear_frac=0.72, lean_deg=5.0, wander=0.20,
                 buttress=0.15,
                 shape="ovoid", radius_m=3.8, height_frac=0.28,
                 center_frac=0.86, shell_upper=0.55, shell_lower=0.35,
                 asymmetry=0.35, offset=0.24, points=1200,
                 model="whorl", step_m=0.30, influence_m=2.8, kill_m=0.55,
                 gravity=-0.12, inertia=0.52, jitter=0.10, max_iter=300,
                 tip_radius_m=0.06, radius_exp=2.4,
                 whorl_count=7, whorl_branches=5, whorl_droop=0.45,
                 whorl_rise=0.30, whorl_sub=3, whorl_irregular=0.42,
                 whorl_leader=0.03,
                 clump_radius_m=0.58, density=0.66, habit="tuft",
                 stretch=2.8, coverage=0.90, separation=1.60,
                 clump_jitter=0.36, squash_f=0.68,
                 mat_bark="bark", mat_leaf="leaf_needle",
                 bio_beach=1.0, bio_grassland=0.4,
                 bio_temperate_forest=0.2,
                 place_abundance=0.6, place_spacing_m=6.0, place_cluster=0.85,
                 place_elev_max_m=400, place_slope_max_pct=50)),
    ),
    "beach-hibiscus": (
        "6 m - a low sprawling mass of big round leaves at the strandline",
        base(name="beach-hibiscus", height_m=6.0,
             notes="BUILT ON THE `sea-grape` FINDING: big round leathery leaves "
                   "are drawn as unusually LARGE clumps on a small tree, because "
                   "clump size is the only handle the lattice gives on leaf "
                   "size. 0.70 m clumps on a 6 m plant, at coverage 0.94, so the "
                   "crown is a solid mass of coarse round units rather than a "
                   "fine canopy.\n\n"
                   "Wider than it is tall and leaning hard (crown radius 3.8 m, "
                   "`trunk.lean_deg` 12, wander 0.50), which is what happens to "
                   "anything that grows in salt wind at the top of a beach -- "
                   "the same shape argument the sea grape makes, one metre "
                   "taller and denser. THE YELLOW FLOWERS ARE NOT HERE: the only "
                   "flower material is `leaf_blossom`, a white-pink, and using "
                   "it would turn the whole crown the wrong colour to gain a "
                   "detail that a scattering of blooms would not read as at this "
                   "size anyway.",
             **t(radius_base_m=0.24, clear_frac=0.12, lean_deg=12.0, wander=0.50,
                 buttress=0.30,
                 shape="umbrella", radius_m=3.8, height_frac=0.82,
                 center_frac=0.58, shell_upper=0.72, shell_lower=0.55,
                 asymmetry=0.48, offset=0.30, points=1300,
                 model="colonize", step_m=0.20, influence_m=1.8, kill_m=0.34,
                 gravity=-0.08, inertia=0.34, jitter=0.18, max_iter=260,
                 tip_radius_m=0.05, radius_exp=2.1,
                 clump_radius_m=0.70, density=0.80, habit="spiral",
                 stretch=1.6, coverage=0.94, separation=1.50,
                 clump_jitter=0.32, squash_f=0.85,
                 mat_bark="bark_pale", mat_leaf="leaf_jungle",
                 bio_beach=1.0, bio_rainforest=0.25,
                 place_abundance=0.5, place_spacing_m=4.0, place_cluster=0.85,
                 place_water_max_m=30, place_elev_max_m=20)),
    ),
    # --- rainforest and savanna: the two shapes nothing shipped covers -------
    "strangler-fig": (
        "25 m - a base of fused arching ridges under a heavy dark dome",
        base(name="strangler-fig", height_m=25.0,
             notes="THE HOLLOW BASKET IS THE SPECIES AND IT IS NOT IN HERE. The "
                   "rainforest file suggested trying a very heavy `roots` "
                   "setting before asking for a generator, and this is that "
                   "experiment written down: `roots.count` at 16 of 16 and "
                   "`roots.rise` at 1.50 of 1.50 -- both parameters at their "
                   "ceiling, which nothing else in the library does -- with "
                   "`trunk.buttress` at 1.0 and the widest trunk of any tree "
                   "this height. What that gives is a BASE MADE OF FUSED "
                   "ARCHING RIDGES rather than a post, which is genuinely "
                   "closer than anything shipped.\n\n"
                   "What it does not give is the hollow. `roots` radiate "
                   "outward from the base and run back down to the ground; a "
                   "fig's basket is a cage of roots that grew DOWNWARD around a "
                   "host trunk which then rotted out, leaving ragged holes "
                   "through a shell with nothing inside it. That is a different "
                   "structure -- a hollow vertical lattice, not a set of ridges "
                   "-- and it is a generator ask, recorded here as one rather "
                   "than faked with a fatter trunk.",
             **t(radius_base_m=1.20, clear_frac=0.30, lean_deg=3.0, wander=0.30,
                 buttress=1.0,
                 shape="sphere", radius_m=7.0, height_frac=0.58,
                 center_frac=0.70, shell_upper=0.78, shell_lower=0.45,
                 asymmetry=0.36, offset=0.22, points=1900,
                 model="colonize", step_m=0.30, influence_m=2.6, kill_m=0.46,
                 gravity=-0.08, inertia=0.44, jitter=0.12, max_iter=300,
                 shade=0.50, tip_radius_m=0.05, radius_exp=2.4,
                 roots_count=16, roots_length_m=4.5, roots_rise=1.50,
                 roots_thickness=0.85, roots_irregular=0.55,
                 clump_radius_m=0.70, density=0.78, habit="spiral",
                 stretch=2.2, coverage=0.94, separation=1.55,
                 clump_jitter=0.30, top_bias=0.45, squash_f=0.85,
                 mat_bark="bark_pale", mat_leaf="leaf_jungle",
                 bio_rainforest=1.0,
                 place_abundance=0.25, place_spacing_m=20.0,
                 place_cluster=0.3, place_slope_max_pct=40,
                 place_elev_max_m=900)),
    ),
    "sausage-tree": (
        "15 m - thick low boughs under a broad heavy riverine crown",
        base(name="sausage-tree", height_m=15.0,
             notes="THE FRUIT IS THE NAME AND THE THIRD SPECIES TO ASK FOR A "
                   "FRUIT PASS. A long heavy cylinder hanging on a cord well "
                   "clear of the foliage is a shape the generator has no "
                   "primitive for: `strand` hangs thin WOOD carrying the crown's "
                   "own leaf material, so a strand set would read as broken "
                   "twigs or as a small willow, and neither is a sausage tree. "
                   "Rowan and crab apple ask for fruit as colour in a crown; "
                   "this one asks for a mass on a cord, which is the harder and "
                   "more valuable version -- it hangs BELOW the crown outline, "
                   "so it changes the silhouette rather than the surface.\n\n"
                   "The crown is authored as the biome file describes it: broad, "
                   "heavy and low-branched, on the bur oak's thick-limb recipe "
                   "(`radius_exp` 2.7, long segments, stubby ends) at savanna "
                   "scale, and placed along watercourses at `water_max_m` 60 "
                   "like the `jackalberry` it shares a riverbank with.",
             **t(radius_base_m=0.55, clear_frac=0.24, lean_deg=5.0, wander=0.24,
                 buttress=0.40,
                 shape="sphere", radius_m=6.0, height_frac=0.70,
                 center_frac=0.64, shell_upper=0.72, shell_lower=0.48,
                 squash=0.85, asymmetry=0.40, offset=0.24, points=1800,
                 model="colonize", step_m=0.32, influence_m=2.8, kill_m=0.52,
                 gravity=-0.04, inertia=0.44, jitter=0.12, max_iter=300,
                 shade=0.45, tip_radius_m=0.07, radius_exp=2.7,
                 clump_radius_m=0.70, density=0.72, habit="opposite",
                 stretch=2.2, coverage=0.88, separation=1.65,
                 clump_jitter=0.34, top_bias=0.40, squash_f=0.85,
                 mat_bark="bark_pale", mat_leaf="leaf_broadleaf",
                 bio_savanna=1.0, bio_grassland=0.2,
                 place_abundance=0.35, place_spacing_m=15.0,
                 place_cluster=0.4, place_water_max_m=60,
                 place_slope_max_pct=40)),
    ),
}


def main() -> int:
    force = seedspec.parse_force(sys.argv[1:])
    seedspec.announce(force, "tree specs")
    written = 0
    for name, (blurb, changes) in SPECIES.items():
        s, rep = sm.patch(sm.default_spec(), changes)
        if seedspec.write(s, SPECS / f"{name}.json", rep.warnings, force=force,
                          label=name, width=26):
            written += 1
        print(f"  {'':<26} {blurb}")
    print(f"\n{written} of {len(SPECIES)} written")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

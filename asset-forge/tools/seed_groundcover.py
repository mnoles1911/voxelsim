"""Author the ground layer: prairie and steppe grasses, dune grasses, the
forest floor, and the tall waterside reeds.

WHY THIS MATTERS AS MUCH AS THE FLOWERS. `docs/biomes/README.md` §7 puts
grassland grasses second and the temperate forest floor third, and the third one
is a CORRECTNESS gap rather than a volume one: a temperate forest floor made of
meadow grass is the wrong biome. A forest floor is ferns and moss. Before this
pass the library had four grass specs and three reeds for the whole world, and
`meadow-grass` was carrying grassland, temperate forest, beach and taiga on its
own.

ALL AT 5 cm, the detail lattice ground cover already uses. `tools/lattice_ab.py`
measured what that costs: reeds lose almost nothing because a 2 m stem has
voxels to spare either way, and grass stops being individual blades and becomes
a chunky vegetation clump. That is a different look, not a broken one, and it is
what these are authored FOR rather than shrunk into -- fewer and wider stems
than a fine-lattice tuft would use, because at 5 cm a blade is one voxel wide
whatever you ask for and thirty of them rooted in a 5 cm disc fuse into a plate.

THE SIZE FLOOR AND WHO IT BINDS. Nothing here is under 0.22 m. Eleven species
are genuinely lower than that in life -- buffalo grass, sheep's fescue, sea
sandwort, hair-cap moss, sphagnum, feather moss, reindeer lichen, crowberry,
woolly fringe-moss, spikemoss and the ivy layer -- and each says so in its own
`notes`. At 5 cm a 6 cm moss carpet is ONE VOXEL, which is not an object, it is
the top of the ground; the temperate-forest file asks whether a continuous mat
should be a terrain material instead, and that question is still open. What is
authored here is the compromise the library already uses everywhere else:
author it to read, and write down that you did.

WHAT A FERN IS IN THIS GENERATOR, STATED PLAINLY. The tuft generator makes a
spray of thin stems from a root crown. It does not make a pinnate frond, and
nothing here pretends otherwise: a fern is authored as a shuttlecock of WIDE,
strongly arched, low-taper stems, which gives the right silhouette and the right
volume and has no leaflets in it. At 5 cm a frond's leaflets are sub-voxel
anyway, so the loss is smaller than it sounds -- but it is a real approximation
and it belongs in this docstring rather than in a surprise.

`tuft.base_m` is never smaller than `tuft.spread_m` on anything here, because
the root crown is what makes a clump ONE PIECE at 26-connectivity and that is
what `tools/buildcheck.py` enforces.

    python tools/seed_groundcover.py
    python tools/seed_groundcover.py --force

SIZES ARE APPROXIMATE AND SAY SO. Every height is the approximate figure from
the biome file it came from; `docs/biomes/README.md` §8 is explicit that those
are unsourced general-knowledge estimates. Nothing here is quoted as measured.
"""
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
import seedspec
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"


def base(kind, **over):
    changes = {
        "kind": kind,
        "resolution_cm": "5",
        "variation.amount": 1.0,
        "variation.height": 0.28,
        "variation.shape": 0.18,
        "variation.proportion": 0.20,
    }
    changes.update(over)
    return changes


def t(**kw):
    """`tuft.*`, `materials.*`, `biomes.*`, `placement.*` from keywords."""
    out = {}
    for k, v in kw.items():
        if k.startswith("mat_"):
            out["materials." + k[len("mat_"):]] = v
        elif k.startswith("bio_"):
            out["biomes." + k[len("bio_"):]] = v
        elif k.startswith("place_"):
            out["placement." + k[len("place_"):]] = v
        else:
            out["tuft." + k] = v
    return out


# name -> (blurb, changes)
SPECIES = {
    # --- prairie and steppe: grassland is 28.06% of the world's land ---------
    "big-bluestem": (
        "grass 2.0 m - the tallgrass prairie one, taller than a person",
        base("grass", name="big-bluestem", height_m=2.0,
             notes="THE TALLEST GRASS IN THE LIBRARY, and the reason the "
                   "prairie needs its own species rather than a scaled meadow "
                   "tuft: a 2 m grass is a wall you cannot see over, and a 45 cm "
                   "one is a lawn. Stiff and near-vertical, with a seed head "
                   "carried well clear.\n\n"
                   "The three-part 'turkey foot' head that names it is three "
                   "spikes 10 cm apart at the very top; at 5 cm that is two "
                   "voxels of separation and it will not read. Drawn as one "
                   "spike, which is honest, and the height carries the species "
                   "without it.",
             **t(stems=11, spread_m=0.14, splay_deg=11, arc=0.20, width_m=0.06,
                 taper=0.5, wander=0.25, length_var=0.35, base_m=0.14,
                 head="spike", head_m=0.16, head_frac=0.16, head_share=0.55,
                 mat_stem="savanna_grass", mat_head="leaf_dry",
                 bio_grassland=1.0, bio_savanna=0.5,
                 place_abundance=0.8, place_spacing_m=1.0, place_cluster=0.8,
                 place_slope_max_pct=40)),
    ),
    "little-bluestem": (
        "grass 0.90 m - dense copper bunch with fluffy white seed",
        base("grass", name="little-bluestem", height_m=0.90,
             notes="The other half of the prairie, and deliberately the "
                   "opposite build to the big bluestem beside it: a dense tight "
                   "bunch rather than a scatter of tall canes. A PLUME head "
                   "rather than a spike, because the white seed is fluffy and a "
                   "spike would read as a reed.",
             **t(stems=18, spread_m=0.10, splay_deg=20, arc=0.34, width_m=0.05,
                 taper=0.45, wander=0.30, length_var=0.35, base_m=0.10,
                 head="plume", head_m=0.14, head_frac=0.18, head_share=0.35,
                 mat_stem="leaf_autumn", mat_head="plume_white",
                 bio_grassland=1.0, bio_savanna=0.35,
                 place_abundance=0.9, place_spacing_m=0.6, place_cluster=0.75)),
    ),
    "switchgrass": (
        "grass 1.50 m - upright clump under an open airy seed haze",
        base("grass", name="switchgrass", height_m=1.50,
             notes="The seed panicle is a HAZE rather than a head -- very open, "
                   "no solid outline -- so it is drawn as a wide low-density "
                   "plume on only a third of the stems. Widening the plume and "
                   "dropping the share is the closest the generator gets to "
                   "'airy', and it is what separates this from the timothy's "
                   "hard cylinder.",
             **t(stems=13, spread_m=0.12, splay_deg=13, arc=0.24, width_m=0.055,
                 taper=0.5, wander=0.28, length_var=0.40, base_m=0.12,
                 head="plume", head_m=0.24, head_frac=0.22, head_share=0.35,
                 mat_stem="grass", mat_head="leaf_dry",
                 bio_grassland=1.0, bio_savanna=0.3, bio_beach=0.2,
                 place_abundance=0.75, place_spacing_m=0.8, place_cluster=0.75)),
    ),
    "buffalo-grass": (
        "grass 0.22 m - fine grey-green shortgrass turf",
        base("grass", name="buffalo-grass", height_m=0.22,
             notes="AUTHORED AT 0.22 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.15 m. At 5 cm 0.15 m is three voxels and a turf-forming "
                   "grass has no shape at three voxels; 0.22 is four to five, "
                   "which is the floor at which a blade can arc. Recorded so it "
                   "is not corrected back.\n\n"
                   "THE SHORTGRASS PRAIRIE FLOOR, and the point is that it is "
                   "the SOD: very tight spacing and near-maximum clustering, so "
                   "placement lays it down as continuous turf rather than as "
                   "scattered tufts. That is what a shortgrass plain looks "
                   "like and no other grass spec in the library asks for it.",
             **t(stems=22, spread_m=0.09, splay_deg=34, arc=0.72, width_m=0.05,
                 taper=0.5, wander=0.45, length_var=0.30, base_m=0.09,
                 head="none", mat_stem="savanna_grass",
                 bio_grassland=1.0, bio_savanna=0.4, bio_desert=0.25,
                 place_abundance=1.0, place_spacing_m=0.5, place_cluster=0.95,
                 place_slope_max_pct=50)),
    ),
    "feather-grass": (
        "grass 0.80 m - fine tussock with long silky streaming awns",
        base("grass", name="feather-grass", height_m=0.80,
             notes="THE AWNS ARE THE SPECIES: long silky threads streaming off "
                   "the seed heads, which at 5 cm cannot be threads. They are "
                   "drawn as a long narrow PLUME instead -- `head_frac` 0.34, "
                   "which is the longest head on any grass here -- so the top "
                   "third of the plant is pale and soft-edged and the bottom "
                   "two thirds is fine blade. That is the read, and it is a "
                   "stylisation rather than the real structure.",
             **t(stems=16, spread_m=0.10, splay_deg=18, arc=0.40, width_m=0.05,
                 taper=0.4, wander=0.35, length_var=0.45, base_m=0.10,
                 head="plume", head_m=0.10, head_frac=0.34, head_share=0.5,
                 mat_stem="savanna_grass", mat_head="plume_white",
                 bio_grassland=1.0, bio_desert=0.3, bio_savanna=0.3,
                 place_abundance=0.7, place_spacing_m=0.9, place_cluster=0.7)),
    ),
    "sheeps-fescue": (
        "grass 0.24 m - very fine blue-green hemispherical cushion",
        base("grass", name="sheeps-fescue", height_m=0.24,
             notes="AUTHORED AT 0.24 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.20 m, to clear the five-voxel floor with an arc left in "
                   "it. Written down so it stays.\n\n"
                   "A CUSHION, which is a very high stem count on a narrow root "
                   "spread with a steep splay and a hard arc -- so the tuft is "
                   "a dome rather than a fan. It is the tightest thing on this "
                   "page and the counterpart to the loose meadow tuft.",
             **t(stems=26, spread_m=0.07, splay_deg=42, arc=0.80, width_m=0.05,
                 taper=0.5, wander=0.50, length_var=0.22, base_m=0.07,
                 head="none", mat_stem="leaf_needle",
                 bio_grassland=0.9, bio_tundra_alpine=0.5, bio_beach=0.3,
                 place_abundance=0.8, place_spacing_m=0.5, place_cluster=0.9,
                 place_slope_max_pct=60)),
    ),
    "cocksfoot": (
        "grass 1.00 m - coarse tussock with a lopsided clenched seed head",
        base("grass", name="cocksfoot", height_m=1.00,
             notes="Coarse, and the head is a hard lumpy one-sided cluster "
                   "rather than a spike or a plume -- so it is drawn as a short "
                   "fat spike with a low share, which at eight voxels of head "
                   "is a knot on the end of the stem. The roughest grass here "
                   "and the one that makes a neglected field look neglected.",
             **t(stems=14, spread_m=0.12, splay_deg=22, arc=0.36, width_m=0.065,
                 taper=0.45, wander=0.30, length_var=0.40, base_m=0.12,
                 head="spike", head_m=0.14, head_frac=0.12, head_share=0.4,
                 mat_stem="grass", mat_head="savanna_grass",
                 bio_grassland=1.0, bio_temperate_forest=0.4,
                 place_abundance=0.8, place_spacing_m=0.7, place_cluster=0.6)),
    ),
    "timothy": (
        "grass 0.90 m - stiff single stems each with a long cylindrical head",
        base("grass", name="timothy", height_m=0.90,
             notes="ONE HEAD PER STEM AND EVERY STEM STRAIGHT, which is the "
                   "cleanest silhouette any grass here has: a long dense "
                   "cylinder held dead vertical. `head_share` near 1 and `arc` "
                   "near 0. It exists to contrast with the switchgrass haze "
                   "beside it -- same height, opposite head.",
             **t(stems=12, spread_m=0.09, splay_deg=8, arc=0.10, width_m=0.05,
                 taper=0.55, wander=0.16, length_var=0.30, base_m=0.09,
                 head="spike", head_m=0.09, head_frac=0.24, head_share=0.85,
                 mat_stem="grass", mat_head="savanna_grass",
                 bio_grassland=1.0, bio_temperate_forest=0.35,
                 place_abundance=0.7, place_spacing_m=0.6, place_cluster=0.6)),
    ),
    "sedge-tussock": (
        "grass 0.60 m - a rising mound of its own dead leaves with living tops",
        base("grass", name="sedge-tussock", height_m=0.60,
             notes="AN OLD TUSSOCK IS HALF DEAD MATTER, which is the thing "
                   "worth drawing: this is authored with a very wide root crown "
                   "and a heavy arc so the outer stems lie over into a skirt, "
                   "and the stem material is the dry one. The living blades on "
                   "top are the shorter, straighter, less arched fraction that "
                   "`length_var` produces.",
             **t(stems=24, spread_m=0.16, splay_deg=30, arc=0.62, width_m=0.055,
                 taper=0.45, wander=0.40, length_var=0.50, base_m=0.16,
                 head="none", mat_stem="leaf_dry",
                 bio_grassland=0.9, bio_taiga=0.6, bio_temperate_forest=0.4,
                 bio_tundra_alpine=0.4,
                 place_abundance=0.6, place_spacing_m=1.0, place_cluster=0.7,
                 place_water_max_m=30)),
    ),
    "ribwort-plantain": (
        "grass 0.30 m - flat ribbed rosette with bare stalks and brown heads",
        base("grass", name="ribwort-plantain", height_m=0.30,
             notes="A ROSETTE, not a tuft: broad ribbed lance leaves laid right "
                   "over on a wide crown, with a few bare stalks standing up "
                   "carrying a short dark head. Filed as `grass` because it is "
                   "a trodden-ground layer plant rather than a flower, and "
                   "because that is where placement wants it.",
             **t(stems=12, spread_m=0.11, splay_deg=54, arc=0.80, width_m=0.08,
                 taper=0.45, wander=0.35, length_var=0.45, base_m=0.11,
                 head="spike", head_m=0.07, head_frac=0.14, head_share=0.3,
                 mat_stem="leaf_broadleaf", mat_head="leaf_dry",
                 bio_grassland=1.0, bio_temperate_forest=0.3, bio_beach=0.25,
                 place_abundance=0.7, place_spacing_m=0.5, place_cluster=0.7)),
    ),
    # --- savanna: 20.76% of land, second largest -----------------------------
    "red-oat-grass": (
        "grass 0.80 m - the signature savanna grass, cured red-brown",
        base("grass", name="red-oat-grass", height_m=0.80,
             notes="ONE SPEC THAT CHANGES THE COLOUR OF A FIFTH OF THE WORLD'S "
                   "LAND. Savanna is seen as a plane of grass with things "
                   "standing on it, so the grass is most of the biome's "
                   "surface, and the only savanna grasses shipped were a pale "
                   "tussock and a pampas plume.\n\n"
                   "Fine arching blades in loose tufts with the awned seed "
                   "heads leaning all one way, which is `arc` high on a low "
                   "splay plus a long light spike.",
             **t(stems=15, spread_m=0.11, splay_deg=20, arc=0.55, width_m=0.05,
                 taper=0.4, wander=0.35, length_var=0.40, base_m=0.11,
                 head="spike", head_m=0.10, head_frac=0.22, head_share=0.45,
                 mat_stem="leaf_autumn", mat_head="leaf_dry",
                 bio_savanna=1.0, bio_grassland=0.5, bio_desert=0.2,
                 place_abundance=1.0, place_spacing_m=0.5, place_cluster=0.7,
                 place_slope_max_pct=45)),
    ),
    "floodplain-sedge": (
        "grass 0.70 m - stiff triangular stems, sharply upright, in wet ground",
        base("grass", name="floodplain-sedge", height_m=0.70,
             notes="Sedges stand where grasses arch, and that is the whole "
                   "difference at eight voxels: near-zero splay, near-zero arc, "
                   "leaves only near the base, and a small dark head on top. "
                   "Placed against water and flat ground.",
             **t(stems=13, spread_m=0.10, splay_deg=6, arc=0.10, width_m=0.055,
                 taper=0.55, wander=0.18, length_var=0.30, base_m=0.10,
                 head="spike", head_m=0.07, head_frac=0.12, head_share=0.6,
                 mat_stem="grass", mat_head="leaf_autumn",
                 bio_savanna=1.0, bio_grassland=0.5, bio_rainforest=0.3,
                 place_abundance=0.6, place_spacing_m=0.6, place_cluster=0.9,
                 place_water_max_m=10, place_slope_max_pct=12)),
    ),
    # --- beach: 5.54% of land but it wraps every shore in the world ----------
    "marram-grass": (
        "grass 0.90 m - stiff in-rolled blue-green blades on bare dune sand",
        base("grass", name="marram-grass", height_m=0.90,
             notes="THE CLUMP GAPS ARE THE DUNE. Marram grows in scattered "
                   "clumps with bare sand between them, and that spacing is the "
                   "look -- so `cluster` is deliberately LOW where every other "
                   "ground cover here is high, and the spacing is wide. Getting "
                   "that wrong gives a lawn on a beach.\n\n"
                   "Stiff and sharply in-rolled: a low arc, a hard taper and a "
                   "narrow splay, so the blades stand and spike rather than "
                   "lying over the way meadow grass does. The single cheapest "
                   "fix for bare sand, which the beach file calls the most "
                   "conspicuous emptiness in the biome.",
             **t(stems=13, spread_m=0.12, splay_deg=16, arc=0.28, width_m=0.055,
                 taper=0.35, wander=0.28, length_var=0.40, base_m=0.12,
                 head="none", mat_stem="leaf_needle",
                 bio_beach=1.0, bio_grassland=0.15,
                 place_abundance=0.8, place_spacing_m=1.1, place_cluster=0.35,
                 place_elev_max_m=25, place_slope_max_pct=55)),
    ),
    "lyme-grass": (
        "grass 1.00 m - wide flat blue-grey blades with a stiff wheat-like head",
        base("grass", name="lyme-grass", height_m=1.00,
             notes="Coarser than marram in every direction: wider blades, "
                   "taller, and it carries a hard head where marram carries "
                   "none. The two share a dune and must not read as one plant, "
                   "so the width is nearly doubled and the head is the "
                   "separator.",
             **t(stems=10, spread_m=0.13, splay_deg=18, arc=0.32, width_m=0.09,
                 taper=0.4, wander=0.25, length_var=0.35, base_m=0.13,
                 head="spike", head_m=0.10, head_frac=0.16, head_share=0.5,
                 mat_stem="leaf_needle", mat_head="leaf_dry",
                 bio_beach=1.0,
                 place_abundance=0.5, place_spacing_m=1.4, place_cluster=0.4,
                 place_elev_max_m=20)),
    ),
    "sea-couch": (
        "grass 0.50 m - wiry blue-green turf on the upper beach",
        base("grass", name="sea-couch", height_m=0.50,
             notes="The continuous one: where marram is clumps with sand "
                   "between, this is turf, so the spacing is tight and the "
                   "clustering high. It is what covers the ground BEHIND the "
                   "foredune, and having both is what makes a beach read as a "
                   "sequence rather than a texture.",
             **t(stems=17, spread_m=0.09, splay_deg=26, arc=0.50, width_m=0.05,
                 taper=0.5, wander=0.40, length_var=0.30, base_m=0.09,
                 head="none", mat_stem="grass",
                 bio_beach=1.0, bio_grassland=0.25,
                 place_abundance=0.9, place_spacing_m=0.5, place_cluster=0.85,
                 place_elev_max_m=30)),
    ),
    "sand-sedge": (
        "grass 0.25 m - low stiff blades, and it grows in straight lines",
        base("grass", name="sand-sedge", height_m=0.25,
             notes="THE IDENTIFYING FEATURE IS INVISIBLE IN THE ASSET AND LIVES "
                   "IN PLACEMENT. Sand sedge runs a rhizome under bare sand and "
                   "sends up shoots in DEAD STRAIGHT LINES across it. Nothing "
                   "in one tuft can say that; what this spec can do is ask for "
                   "it -- very tight spacing with maximum clustering, so a "
                   "scatterer that respects `cluster` produces runs rather than "
                   "clouds. Recorded here because a future scatter feature is "
                   "what finishes this species, not a retune of the tuft.",
             **t(stems=9, spread_m=0.07, splay_deg=14, arc=0.30, width_m=0.055,
                 taper=0.5, wander=0.22, length_var=0.30, base_m=0.07,
                 head="none", mat_stem="savanna_grass",
                 bio_beach=1.0,
                 place_abundance=0.5, place_spacing_m=0.5, place_cluster=1.0,
                 place_elev_max_m=15)),
    ),
    "glasswort": (
        "grass 0.25 m - leafless jointed succulent fingers on salt mud",
        base("grass", name="glasswort", height_m=0.25,
             notes="A STACK OF BEADS, NOT BLADES, and the only thing on this "
                   "page that is not a leaf. Drawn as very few, very thick, "
                   "near-vertical, barely tapered stems -- `taper` 0.9 is the "
                   "highest here, so the stem is the same thickness at the top "
                   "as at the bottom, which is what a succulent finger is. The "
                   "jointing is 1 cm and cannot exist at 5.",
             **t(stems=8, spread_m=0.08, splay_deg=22, arc=0.20, width_m=0.075,
                 taper=0.9, wander=0.25, length_var=0.35, base_m=0.08,
                 head="none", mat_stem="leaf_jungle",
                 bio_beach=1.0,
                 place_abundance=0.7, place_spacing_m=0.5, place_cluster=0.95,
                 place_water_max_m=15, place_elev_max_m=5,
                 place_slope_max_pct=8)),
    ),
    "sea-purslane": (
        "grass 0.30 m - low grey-green fleshy mat on marsh creek edges",
        base("grass", name="sea-purslane", height_m=0.30,
             notes="A fleshy MAT, so wide spread, high splay, hard arc and "
                   "thick stems. It lines salt-marsh creeks, which is a "
                   "placement statement more than a shape one: near water, "
                   "very flat ground, very low elevation, heavily clustered.",
             **t(stems=18, spread_m=0.13, splay_deg=52, arc=0.82, width_m=0.07,
                 taper=0.6, wander=0.50, length_var=0.30, base_m=0.13,
                 head="none", mat_stem="leaf_needle",
                 bio_beach=1.0,
                 place_abundance=0.6, place_spacing_m=0.6, place_cluster=0.95,
                 place_water_max_m=8, place_elev_max_m=6,
                 place_slope_max_pct=10)),
    ),
    "sea-sandwort": (
        "grass 0.22 m - tight bright green fleshy rosettes creeping over sand",
        base("grass", name="sea-sandwort", height_m=0.22,
             notes="AUTHORED AT 0.22 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.10 m. At 5 cm 0.10 m is two voxels: a smear, not a "
                   "rosette. Written down so it is not shrunk back.\n\n"
                   "The first colonist of bare sand above the tide, so it is "
                   "authored to sit alone rather than in sheets -- low cluster, "
                   "wide spacing, and a strongly domed shape from a high stem "
                   "count on a steep splay.",
             **t(stems=20, spread_m=0.10, splay_deg=56, arc=0.85, width_m=0.06,
                 taper=0.65, wander=0.50, length_var=0.22, base_m=0.10,
                 head="none", mat_stem="leaf_jungle",
                 bio_beach=1.0,
                 place_abundance=0.4, place_spacing_m=1.0, place_cluster=0.45,
                 place_elev_max_m=8)),
    ),
    # --- temperate forest floor: the correctness gap ------------------------
    "bracken": (
        "grass 1.50 m - coarse triangular fronds in a uniform waist-high stand",
        base("grass", name="bracken", height_m=1.50,
             notes="THE ONE THAT EXCLUDES EVERYTHING ELSE. Bracken grows in "
                   "uniform stands that carry no understorey at all, so it is "
                   "authored with maximum clustering and very tight spacing: a "
                   "patch of it should be a solid waist-high sheet with a hard "
                   "edge, not a scatter.\n\n"
                   "FERN CAVEAT, STATED ONCE FOR ALL FIVE FERNS HERE. The tuft "
                   "generator makes stems, not pinnate fronds. A fern is drawn "
                   "as wide, low-taper, strongly arched stems on a bare lower "
                   "stalk -- right silhouette, right volume, no leaflets. At "
                   "5 cm a leaflet is sub-voxel anyway, so the loss is smaller "
                   "than it sounds, but it is an approximation and not a "
                   "rendering of the plant.",
             **t(stems=9, spread_m=0.14, splay_deg=20, arc=0.52, width_m=0.11,
                 taper=0.30, wander=0.25, length_var=0.30, base_m=0.14,
                 head="none", mat_stem="leaf_broadleaf",
                 bio_temperate_forest=1.0, bio_grassland=0.45, bio_taiga=0.35,
                 place_abundance=0.9, place_spacing_m=0.7, place_cluster=1.0,
                 place_slope_max_pct=50)),
    ),
    "lady-fern": (
        "grass 0.90 m - delicate finely divided shuttlecock rosette",
        base("grass", name="lady-fern", height_m=0.90,
             notes="The SHUTTLECOCK: fronds leaving the crown steeply and "
                   "arching right over, which is a low splay with a very high "
                   "arc -- the opposite of a grass tuft's low-and-out. Finer "
                   "and more arched than the sword fern beside it, which is "
                   "stiff and upright; those two settings are the whole "
                   "difference between the two ferns at eighteen voxels.",
             **t(stems=11, spread_m=0.09, splay_deg=14, arc=0.78, width_m=0.09,
                 taper=0.30, wander=0.22, length_var=0.28, base_m=0.09,
                 head="none", mat_stem="leaf_broadleaf",
                 bio_temperate_forest=1.0, bio_taiga=0.4, bio_rainforest=0.3,
                 place_abundance=0.7, place_spacing_m=0.8, place_cluster=0.85,
                 place_water_max_m=60)),
    ),
    "sword-fern": (
        "grass 1.20 m - stiff dark leathery fronds in a dense upright rosette",
        base("grass", name="sword-fern", height_m=1.20,
             notes="The Pacific forest floor, and the STIFF fern: fronds held "
                   "up and only arching at the tip, so `arc` is half the lady "
                   "fern's on a taller plant. Leathery and once-divided rather "
                   "than lacy, which here is a thicker stem and a harder taper.",
             **t(stems=13, spread_m=0.10, splay_deg=18, arc=0.42, width_m=0.10,
                 taper=0.35, wander=0.18, length_var=0.25, base_m=0.10,
                 head="none", mat_stem="leaf_needle",
                 bio_temperate_forest=1.0, bio_taiga=0.3,
                 place_abundance=0.8, place_spacing_m=0.8, place_cluster=0.9)),
    ),
    "male-fern": (
        "grass 1.20 m - big coarse twice-divided shuttlecock with scaly bases",
        base("grass", name="male-fern", height_m=1.20,
             notes="The big generic woodland fern, between the lady fern's "
                   "delicacy and the sword fern's stiffness, and coarser than "
                   "either: fewer, wider, longer fronds on a bigger crown. It "
                   "is the one that should be commonest, so it carries the "
                   "highest abundance of the three.",
             **t(stems=10, spread_m=0.12, splay_deg=16, arc=0.62, width_m=0.12,
                 taper=0.30, wander=0.22, length_var=0.30, base_m=0.12,
                 head="none", mat_stem="leaf_broadleaf",
                 bio_temperate_forest=1.0, bio_taiga=0.45,
                 place_abundance=0.85, place_spacing_m=0.9, place_cluster=0.8)),
    ),
    "harts-tongue-fern": (
        "grass 0.50 m - the odd one: undivided glossy strap fronds",
        base("grass", name="harts-tongue-fern", height_m=0.50,
             notes="THE ONE FERN THE GENERATOR CAN DRAW HONESTLY, because it is "
                   "not divided: a hart's tongue is a rosette of plain glossy "
                   "straps, which is exactly what a wide low-taper stem is. "
                   "Everything approximate about the other four ferns on this "
                   "page is exact here, and it is worth having for that alone.",
             **t(stems=9, spread_m=0.08, splay_deg=26, arc=0.58, width_m=0.13,
                 taper=0.55, wander=0.20, length_var=0.30, base_m=0.08,
                 head="none", mat_stem="leaf_jungle",
                 bio_temperate_forest=1.0, bio_rainforest=0.25,
                 place_abundance=0.4, place_spacing_m=1.0, place_cluster=0.8,
                 place_water_max_m=40)),
    ),
    "hair-cap-moss": (
        "grass 0.24 m - upright dark stems in a bristly turf",
        base("grass", name="hair-cap-moss", height_m=0.24,
             notes="AUTHORED AT 0.24 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.10 m. At 5 cm the real plant is two voxels. Recorded so "
                   "it is not corrected.\n\n"
                   "A BRISTLE, not a cushion, and that is the whole reason it "
                   "is a separate species from the feather moss beside it: hair "
                   "cap stands upright in a dense turf, so the splay and the "
                   "arc are both near zero and the stem count is the highest on "
                   "the page. Feather moss is a mat and lies over.",
             **t(stems=34, spread_m=0.09, splay_deg=10, arc=0.14, width_m=0.05,
                 taper=0.7, wander=0.30, length_var=0.25, base_m=0.09,
                 head="none", mat_stem="leaf_needle",
                 bio_temperate_forest=0.9, bio_taiga=0.8,
                 bio_tundra_alpine=0.3,
                 place_abundance=0.9, place_spacing_m=0.5, place_cluster=0.95,
                 place_water_max_m=60)),
    ),
    "sphagnum-hummock": (
        "grass 0.28 m - saturated pale bog mound, domed rather than flat",
        base("grass", name="sphagnum-hummock", height_m=0.28,
             notes="AUTHORED AT 0.28 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.15-0.20 m mound, to give the dome somewhere to be at "
                   "5 cm. Written down so it stays.\n\n"
                   "IT BUILDS MOUNDS, NOT A FLAT MAT, which is the one thing "
                   "that separates sphagnum from every other moss: a very high "
                   "stem count on a wide crown with a steep splay and a hard "
                   "arc gives a dome. Bog only -- flat ground, close to water, "
                   "maximum clustering.",
             **t(stems=32, spread_m=0.15, splay_deg=48, arc=0.86, width_m=0.06,
                 taper=0.7, wander=0.45, length_var=0.25, base_m=0.15,
                 head="none", mat_stem="leaf_jungle",
                 bio_taiga=1.0, bio_temperate_forest=0.5,
                 bio_tundra_alpine=0.5,
                 place_abundance=0.9, place_spacing_m=0.5, place_cluster=1.0,
                 place_water_max_m=25, place_slope_max_pct=10)),
    ),
    "feather-moss": (
        "grass 0.22 m - continuous fine gold-green mat between trunks",
        base("grass", name="feather-moss", height_m=0.22,
             notes="AUTHORED AT 0.22 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.05-0.06 m, WHICH IS THE LARGEST DEPARTURE ON THIS PAGE "
                   "AND THE MOST ARGUABLE. At 5 cm a real feather-moss carpet "
                   "is ONE VOXEL, and the temperate-forest file says outright "
                   "that a one-voxel mat is not an object -- it is the top of "
                   "the ground, and the honest implementation may be a terrain "
                   "SURFACE MATERIAL rather than a scattered asset. That "
                   "question is open and this spec does not close it.\n\n"
                   "It is authored anyway because a boreal floor of bare podzol "
                   "between trunks is the main thing that makes a taiga "
                   "screenshot look wrong, and an asset that exists can be "
                   "replaced by a material later. Maximum clustering and the "
                   "tightest spacing in the library, because it is meant to "
                   "tile into a continuous carpet.",
             **t(stems=30, spread_m=0.12, splay_deg=44, arc=0.88, width_m=0.06,
                 taper=0.6, wander=0.50, length_var=0.20, base_m=0.12,
                 head="none", mat_stem="leaf_jungle",
                 bio_taiga=1.0, bio_temperate_forest=0.8,
                 bio_rainforest=0.3, bio_tundra_alpine=0.3,
                 place_abundance=1.0, place_spacing_m=0.5, place_cluster=1.0,
                 place_slope_max_pct=55)),
    ),
    "wood-sedge": (
        "grass 0.50 m - loose arching bright blades with drooping seed spikes",
        base("grass", name="wood-sedge", height_m=0.50,
             notes="The shade sedge: looser and much more arched than the "
                   "savanna floodplain sedge, with the seed spikes hanging "
                   "rather than standing. Same kind, opposite settings, and "
                   "between them they cover both ends of what a sedge looks "
                   "like.",
             **t(stems=14, spread_m=0.09, splay_deg=24, arc=0.72, width_m=0.06,
                 taper=0.4, wander=0.35, length_var=0.40, base_m=0.09,
                 head="spike", head_m=0.06, head_frac=0.14, head_share=0.4,
                 mat_stem="grass", mat_head="leaf_dry",
                 bio_temperate_forest=1.0, bio_taiga=0.35,
                 place_abundance=0.6, place_spacing_m=0.7, place_cluster=0.75)),
    ),
    "dogs-mercury": (
        "grass 0.35 m - dull dark upright unbranched stems in a uniform sheet",
        base("grass", name="dogs-mercury", height_m=0.35,
             notes="The dullest asset in the library on purpose. It is a "
                   "uniform dark sheet under beech and ash with no flowers "
                   "worth drawing, and its whole contribution is that the "
                   "forest floor is COVERED rather than bare. Upright, "
                   "unbranched, closely spaced, maximum clustering.",
             **t(stems=15, spread_m=0.08, splay_deg=12, arc=0.24, width_m=0.07,
                 taper=0.55, wander=0.20, length_var=0.25, base_m=0.08,
                 head="none", mat_stem="leaf_needle",
                 bio_temperate_forest=1.0,
                 place_abundance=0.85, place_spacing_m=0.5, place_cluster=1.0,
                 place_elev_max_m=800)),
    ),
    "bilberry-mat": (
        "grass 0.40 m - low woody green mat with small round leaves",
        base("grass", name="bilberry-mat", height_m=0.40,
             notes="Filed as ground cover rather than as a bush because that is "
                   "what it does: a continuous knee-high layer under pines, not "
                   "an individual shrub. The woody stems are wider and stiffer "
                   "than any grass here and the arc is mid rather than high, so "
                   "it reads as a mass with structure in it.",
             **t(stems=20, spread_m=0.13, splay_deg=38, arc=0.55, width_m=0.07,
                 taper=0.55, wander=0.40, length_var=0.30, base_m=0.13,
                 head="none", mat_stem="leaf_broadleaf",
                 bio_taiga=1.0, bio_temperate_forest=0.7,
                 bio_tundra_alpine=0.4,
                 place_abundance=0.9, place_spacing_m=0.5, place_cluster=0.95)),
    ),
    "wood-horsetail": (
        "grass 0.50 m - whorls of fine drooping branchlets, a miniature conifer",
        base("grass", name="wood-horsetail", height_m=0.50,
             notes="A MINIATURE CONIFER made of tiers of fine drooping "
                   "branchlets. The generator cannot tier a tuft, so what is "
                   "authored is the read the tiers produce: many very fine "
                   "stems on a narrow crown with a hard arc, so the plant is a "
                   "soft drooping cone. Damp ground and shade.",
             **t(stems=26, spread_m=0.07, splay_deg=24, arc=0.80, width_m=0.05,
                 taper=0.4, wander=0.35, length_var=0.35, base_m=0.07,
                 head="none", mat_stem="leaf_jungle",
                 bio_temperate_forest=0.9, bio_taiga=0.6,
                 place_abundance=0.45, place_spacing_m=0.7, place_cluster=0.85,
                 place_water_max_m=30)),
    ),
    "ivy-ground-layer": (
        "grass 0.22 m - dark glossy lobed leaves in a flat sheet",
        base("grass", name="ivy-ground-layer", height_m=0.22,
             notes="AUTHORED AT 0.22 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.15 m, for the five-voxel floor.\n\n"
                   "IT CLIMBS ANYTHING VERTICAL IT MEETS AND THIS ASSET DOES "
                   "NOT. Nothing in `forge/kinds.py` attaches an asset to a "
                   "trunk, so this is the flat sheet only, and the climbing "
                   "half stays an open placement question the same way the "
                   "rainforest's epiphytes do. The sheet is the part worth "
                   "having: it is what covers a wood's floor in winter when "
                   "everything else has died back.",
             **t(stems=22, spread_m=0.14, splay_deg=60, arc=0.88, width_m=0.09,
                 taper=0.6, wander=0.50, length_var=0.25, base_m=0.14,
                 head="none", mat_stem="leaf_jungle",
                 bio_temperate_forest=1.0,
                 place_abundance=0.8, place_spacing_m=0.5, place_cluster=1.0)),
    ),
    # --- taiga ---------------------------------------------------------------
    "reindeer-lichen": (
        "grass 0.24 m - pale grey-green spongy forked cushion",
        base("grass", name="reindeer-lichen", height_m=0.24,
             notes="AUTHORED AT 0.24 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.06-0.08 m, and authored as `grass` rather than waiting "
                   "for `gen: lichen`. Two honest caveats.\n\n"
                   "FIRST, THE BRANCHING IS NOT DRAWN. Real reindeer lichen is "
                   "repeatedly forked hollow tubes at millimetre-to-centimetre "
                   "scale; the tuft generator makes unforked stems, and at 5 cm "
                   "a fork is sub-voxel regardless. What this gives is the "
                   "right pale spongy cushion at the right density, which is "
                   "what the ground of a dry pine forest looks like from "
                   "standing height, and nothing closer is reachable at any "
                   "lattice this project has.\n\n"
                   "SECOND, THE COLOUR IS APPROXIMATE. There is no pale "
                   "grey-green in the stem palette; `leaf_needle` is the "
                   "nearest and it is darker and greener than the real thing. "
                   "That is a materials ask, not a geometry one.",
             **t(stems=30, spread_m=0.11, splay_deg=40, arc=0.72, width_m=0.055,
                 taper=0.8, wander=0.55, length_var=0.30, base_m=0.11,
                 head="none", mat_stem="leaf_needle",
                 bio_taiga=1.0, bio_tundra_alpine=0.8,
                 place_abundance=1.0, place_spacing_m=0.5, place_cluster=0.95,
                 place_slope_max_pct=50)),
    ),
    "crowberry-mat": (
        "grass 0.22 m - dark evergreen needle mat with small black berries",
        base("grass", name="crowberry-mat", height_m=0.22,
             notes="AUTHORED AT 0.22 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.15 m, for the five-voxel floor.\n\n"
                   "Almost a ground texture with relief, which is what the "
                   "tundra file calls it: the darkest thing on the permafrost "
                   "band, laid right over, and tiling. It exists to give the "
                   "alpine ground something other than pale sedge and grey "
                   "lichen.",
             **t(stems=26, spread_m=0.13, splay_deg=58, arc=0.88, width_m=0.05,
                 taper=0.75, wander=0.50, length_var=0.25, base_m=0.13,
                 head="none", mat_stem="leaf_needle",
                 bio_tundra_alpine=1.0, bio_taiga=0.6,
                 place_abundance=0.9, place_spacing_m=0.5, place_cluster=1.0,
                 place_elev_min_m=400, place_slope_max_pct=60)),
    ),
    "woolly-fringe-moss": (
        "grass 0.22 m - grey-green hummock draped over rock edges",
        base("grass", name="woolly-fringe-moss", height_m=0.22,
             notes="AUTHORED AT 0.22 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.10 m.\n\n"
                   "IT DRAPES OVER ROCK RATHER THAN GROWING BESIDE IT, and the "
                   "generator cannot say that -- there is nothing in the tuft "
                   "code that knows a boulder is there. What is authored is the "
                   "hummock: rounded, slightly lumpy on top, greyer than the "
                   "green mosses, and permitted on the steepest ground any "
                   "ground cover here allows so a scatterer can put it on "
                   "broken slopes.",
             **t(stems=28, spread_m=0.12, splay_deg=52, arc=0.84, width_m=0.06,
                 taper=0.7, wander=0.55, length_var=0.30, base_m=0.12,
                 head="none", mat_stem="podzol",
                 bio_tundra_alpine=1.0, bio_taiga=0.5,
                 bio_temperate_forest=0.25,
                 place_abundance=0.7, place_spacing_m=0.5, place_cluster=0.95,
                 place_slope_max_pct=65)),
    ),
    # --- desert --------------------------------------------------------------
    "big-galleta": (
        "grass 0.70 m - coarse wide-spaced blue-green blades on woody bases",
        base("grass", name="big-galleta", height_m=0.70,
             notes="THE RING WITH A DEAD MIDDLE. An old desert bunchgrass dies "
                   "out at the centre and lives on at the rim, which is a very "
                   "wide root spread with a steep splay -- the stems leave the "
                   "crown near its edge and lean out, so the middle is empty. "
                   "Much more open than any temperate tussock.",
             **t(stems=11, spread_m=0.17, splay_deg=38, arc=0.42, width_m=0.065,
                 taper=0.45, wander=0.30, length_var=0.45, base_m=0.17,
                 head="none", mat_stem="leaf_needle",
                 bio_desert=1.0, bio_savanna=0.4, bio_grassland=0.3,
                 place_abundance=0.5, place_spacing_m=1.8, place_cluster=0.4)),
    ),
    "desert-saltgrass": (
        "grass 0.25 m - short stiff grey-green blades in a flat patchy turf",
        base("grass", name="desert-saltgrass", height_m=0.25,
             notes="Sharp, short and stiff, in patches with bare salt crust "
                   "between them -- so `cluster` is high but `abundance` is "
                   "low, which is how a scatterer produces patches rather than "
                   "either a lawn or a scatter. The blades are set in two ranks "
                   "along creeping runners in life; at 5 cm that is a stem "
                   "arrangement nothing can see, so it is drawn as a low stiff "
                   "tuft.",
             **t(stems=16, spread_m=0.09, splay_deg=28, arc=0.32, width_m=0.055,
                 taper=0.35, wander=0.30, length_var=0.25, base_m=0.09,
                 head="none", mat_stem="savanna_grass",
                 bio_desert=1.0, bio_beach=0.3, bio_grassland=0.2,
                 place_abundance=0.45, place_spacing_m=0.5, place_cluster=0.95,
                 place_slope_max_pct=25)),
    ),
    "esparto-grass": (
        "grass 0.90 m - tight in-rolled wiry column standing near-vertical",
        base("grass", name="esparto-grass", height_m=0.90,
             notes="THE WHOLE CLUMP READS AS ONE NARROW COLUMN, which is the "
                   "narrowest root spread on any tall grass here combined with "
                   "the lowest splay: the leaves are in-rolled and near-vertical "
                   "and the clump is a bundle rather than a fan. That column is "
                   "the entire silhouette on a stony North African plain.",
             **t(stems=20, spread_m=0.07, splay_deg=7, arc=0.20, width_m=0.05,
                 taper=0.5, wander=0.20, length_var=0.30, base_m=0.07,
                 head="none", mat_stem="savanna_grass",
                 bio_desert=1.0, bio_grassland=0.35, bio_savanna=0.25,
                 place_abundance=0.6, place_spacing_m=1.2, place_cluster=0.6)),
    ),
    # --- rainforest ----------------------------------------------------------
    "understory-fern": (
        "grass 0.90 m - low shuttlecock of arching flat-planed fronds",
        base("grass", name="understory-fern", height_m=0.90,
             notes="The rainforest file's head-height gap in one spec: the "
                   "shipped list had a canopy and a floor and almost nothing "
                   "between them. Wider and darker than the temperate lady "
                   "fern, with the fronds held flatter -- a flat plane of "
                   "leaflets is what a wet-forest fern presents to the little "
                   "light it gets.\n\n"
                   "Same fern caveat as `bracken`: these are stems, not pinnate "
                   "fronds.",
             **t(stems=12, spread_m=0.11, splay_deg=20, arc=0.68, width_m=0.11,
                 taper=0.30, wander=0.22, length_var=0.30, base_m=0.11,
                 head="none", mat_stem="leaf_jungle",
                 bio_rainforest=1.0, bio_temperate_forest=0.3,
                 place_abundance=0.9, place_spacing_m=0.8, place_cluster=0.85)),
    ),
    "spikemoss-mat": (
        "grass 0.22 m - flat blue-green scale-leaf carpet over soil and rock",
        base("grass", name="spikemoss-mat", height_m=0.22,
             notes="AUTHORED AT 0.22 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.08 m, and carrying the same open question `feather-moss` "
                   "does: the rainforest file says outright that at 5 cm this "
                   "is one or two voxels and cannot be an individual plant, and "
                   "that the honest answer may be a ground material rather than "
                   "an asset. It is authored as a MAT -- one wide low mass with "
                   "no resolved leaves -- which is the file's own first option, "
                   "and it can be replaced by a material later without "
                   "anything downstream noticing.",
             **t(stems=30, spread_m=0.15, splay_deg=64, arc=0.90, width_m=0.06,
                 taper=0.7, wander=0.55, length_var=0.20, base_m=0.15,
                 head="none", mat_stem="leaf_jungle",
                 bio_rainforest=1.0,
                 place_abundance=1.0, place_spacing_m=0.5, place_cluster=1.0,
                 place_water_max_m=80)),
    ),
    # --- reeds: the tall waterside kind --------------------------------------
    "papyrus": (
        "reed 3.50 m - bare triangular stems with a firework of thin rays",
        base("reed", name="papyrus", height_m=3.50,
             notes="A STEM WITH A FIREWORK ON IT, and the tallest ground-layer "
                   "asset in the library. Everything is in the head: a huge "
                   "spherical burst of thin rays on a completely bare stem, so "
                   "`head_m` is the widest here by a long way and `head_share` "
                   "is 1.0 -- there are no leaves anywhere on the plant to draw "
                   "as plain stems.\n\n"
                   "`plume` rather than `spike`, because the burst is radiating "
                   "rays and not a dense cylinder.",
             **t(stems=9, spread_m=0.22, splay_deg=9, arc=0.14, width_m=0.09,
                 taper=0.7, wander=0.20, length_var=0.28, base_m=0.22,
                 head="plume", head_m=0.55, head_frac=0.13, head_share=1.0,
                 mat_stem="leaf_jungle", mat_head="grass",
                 bio_rainforest=1.0, bio_savanna=0.5,
                 place_abundance=0.7, place_spacing_m=1.2, place_cluster=0.95,
                 place_water_max_m=6, place_slope_max_pct=10)),
    ),
    "giant-reed": (
        "reed 4.00 m - stout bamboo-like cane with a pale feather plume",
        base("reed", name="giant-reed", height_m=4.00,
             notes="Oasis and river margin only, which in a desert means it "
                   "places against the three water exceptions the desert file "
                   "lists and nowhere else -- hence the very short "
                   "`water_max_m` and the flat-ground gate. Stout canes with "
                   "broad leaves in two ranks and a pale plume on top; the "
                   "leaves are the plain stems, so `head_share` is well under "
                   "1.",
             **t(stems=11, spread_m=0.24, splay_deg=12, arc=0.22, width_m=0.10,
                 taper=0.6, wander=0.22, length_var=0.30, base_m=0.24,
                 head="plume", head_m=0.34, head_frac=0.15, head_share=0.5,
                 mat_stem="leaf_jungle", mat_head="leaf_dry",
                 bio_desert=1.0, bio_savanna=0.5, bio_grassland=0.3,
                 place_abundance=0.5, place_spacing_m=1.5, place_cluster=0.95,
                 place_water_max_m=5, place_slope_max_pct=10)),
    ),
    "elephant-grass": (
        "reed 3.00 m - a wall of thick canes with broad drooping leaves",
        base("reed", name="elephant-grass", height_m=3.00,
             notes="USED AS A STAND, NOT AS TUFTS, and the placement rows say "
                   "so: maximum clustering with tight spacing, so a patch is a "
                   "solid wall two to three times head height that the player "
                   "cannot see through or over. That is what it is for -- it is "
                   "the only ground-layer asset in the library that blocks a "
                   "view.\n\n"
                   "Broad DROOPING leaves rather than the giant reed's stiffer "
                   "ones, so the arc is higher and the head is absent on most "
                   "stems.",
             **t(stems=14, spread_m=0.22, splay_deg=16, arc=0.44, width_m=0.10,
                 taper=0.45, wander=0.30, length_var=0.35, base_m=0.22,
                 head="plume", head_m=0.22, head_frac=0.12, head_share=0.35,
                 mat_stem="grass", mat_head="leaf_dry",
                 bio_savanna=1.0, bio_rainforest=0.5, bio_grassland=0.3,
                 place_abundance=0.6, place_spacing_m=1.0, place_cluster=1.0,
                 place_water_max_m=40, place_slope_max_pct=25)),
    ),
    "common-cottongrass": (
        "reed 0.50 m - a single white cotton ball on each stiff bare stem",
        base("reed", name="common-cottongrass", height_m=0.50,
             notes="THE WHITE HEADS ARE THE WHOLE VISUAL and the tussock is "
                   "not: on a wet bog seen from any distance this is a scatter "
                   "of white dots at a common height and nothing else. So the "
                   "head is `plume` in true white on almost every stem, and the "
                   "length spread is deliberately low so the dots line up.\n\n"
                   "`plume_white` rather than `snow`: snow has no per-voxel "
                   "jitter at all and reads as a printed decal, which is the "
                   "same finding `materials.py` records for a bird's belly.",
             **t(stems=12, spread_m=0.10, splay_deg=10, arc=0.16, width_m=0.05,
                 taper=0.6, wander=0.22, length_var=0.18, base_m=0.10,
                 head="plume", head_m=0.12, head_frac=0.12, head_share=0.85,
                 mat_stem="grass", mat_head="plume_white",
                 bio_taiga=1.0, bio_tundra_alpine=0.8,
                 bio_temperate_forest=0.25,
                 place_abundance=0.8, place_spacing_m=0.5, place_cluster=0.95,
                 place_water_max_m=15, place_slope_max_pct=10)),
    ),
    "tussock-cottongrass": (
        "reed 0.50 m - dense tussock with the same white heads over it",
        base("reed", name="tussock-cottongrass", height_m=0.50,
             notes="The same white heads on a completely different plant, and "
                   "the pair is the point: the common cottongrass is bare stems "
                   "out of wet moss and this one sits on a hard dense tussock "
                   "of fine blades. Three times the stems, half of them "
                   "headless and arched right over, on a much wider crown. On "
                   "the permafrost band a tussock is a thing you trip over.",
             **t(stems=30, spread_m=0.15, splay_deg=30, arc=0.60, width_m=0.05,
                 taper=0.45, wander=0.40, length_var=0.40, base_m=0.15,
                 head="plume", head_m=0.11, head_frac=0.11, head_share=0.3,
                 mat_stem="leaf_dry", mat_head="plume_white",
                 bio_tundra_alpine=1.0, bio_taiga=0.6,
                 place_abundance=0.7, place_spacing_m=0.7, place_cluster=0.9,
                 place_elev_min_m=300, place_slope_max_pct=20)),
    ),
    "smooth-cordgrass": (
        "reed 1.20 m - coarse bright stiff blades on tidal mud",
        base("reed", name="smooth-cordgrass", height_m=1.20,
             notes="Tidal mud, and the plant that makes a salt marsh look like "
                   "a salt marsh: dense stiff stands right at the water's edge "
                   "with a one-sided seed spike. Filed as `reed` because that "
                   "is what it is -- a tall near-vertical waterside stem with a "
                   "seed head -- rather than because of any code difference.",
             **t(stems=13, spread_m=0.14, splay_deg=9, arc=0.16, width_m=0.07,
                 taper=0.5, wander=0.20, length_var=0.30, base_m=0.14,
                 head="spike", head_m=0.10, head_frac=0.18, head_share=0.6,
                 mat_stem="grass", mat_head="leaf_dry",
                 bio_beach=1.0,
                 place_abundance=0.8, place_spacing_m=0.5, place_cluster=1.0,
                 place_water_max_m=8, place_elev_max_m=5,
                 place_slope_max_pct=8)),
    ),
    "reed-sweet-grass": (
        "reed 1.50 m - broad soft bright green blades arching over wet hollows",
        base("reed", name="reed-sweet-grass", height_m=1.50,
             notes="The SOFT reed, and the one that arches: `water-reed` and "
                   "`bulrush` are both stiff and near-vertical, and a wet "
                   "hollow full of nothing but stiff vertical stems reads as a "
                   "fence. Broad blades, high arc, no head on most stems.",
             **t(stems=14, spread_m=0.16, splay_deg=20, arc=0.58, width_m=0.09,
                 taper=0.4, wander=0.30, length_var=0.35, base_m=0.16,
                 head="spike", head_m=0.12, head_frac=0.14, head_share=0.25,
                 mat_stem="grass", mat_head="leaf_dry",
                 bio_grassland=0.9, bio_temperate_forest=0.7, bio_taiga=0.4,
                 place_abundance=0.7, place_spacing_m=0.7, place_cluster=1.0,
                 place_water_max_m=5, place_slope_max_pct=10)),
    ),
}


def main() -> int:
    force = seedspec.parse_force(sys.argv[1:])
    seedspec.announce(force, "ground-cover specs")
    written = 0
    for name, (blurb, changes) in SPECIES.items():
        s, rep = sm.patch(sm.default_spec(), changes)
        if seedspec.write(s, SPECS / f"{name}.json", rep.warnings, force=force,
                          label=name, width=24):
            written += 1
        print(f"  {'':<24} {blurb}")
    print(f"\n{written} of {len(SPECIES)} written")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

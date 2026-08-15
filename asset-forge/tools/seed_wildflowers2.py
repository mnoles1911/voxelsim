"""Author the rest of the flower lists: fourteen species and one moss.

WHAT IS LEFT AFTER `tools/seed_wildflowers.py`. That file took the forty-seven
rows that were straightforwardly a tuft. What remained in the biome tables is
harder in one of two ways, and both are worth naming because they decide how
these are authored:

  * SEVEN OF THESE ARE UNDER-SIZED PLANTS -- harebell, wood sorrel, dog violet,
    trout lily, cyclamen, devil's thorn, moss cushion. Three of them carry a
    ⚠ in their own biome file reasoning about exactly this, and the reasoning
    is the same each time: the plant is 10-15 cm and its flower is 1-2 cm, so
    on the 5 cm ground-cover lattice the flower is a THIRD OF A VOXEL. The
    house answer already exists and `meadow-daisy` shipped it -- draw the bloom
    as a single voxel of a distinct colour on a one-voxel stem, which means the
    flower is at three to five times life size. Every one of them says so in
    its `notes`, because the note is the only thing standing between the next
    reader and a well-meant "correction" back to the size in the table.
  * SEVEN ARE TWO-PART PLANTS -- a flat leaf mat or a pair of broad leaves,
    with something quite different standing out of it: lily of the valley's one
    arched bell stem, cyclamen's bare upswept stalks, bugle's blue spikes,
    trout lily's single nodding flower. The tuft generator has ONE arc, ONE
    splay and ONE width for the whole clump, so the two halves cannot be
    authored separately. What can be authored is the SHARE -- `tuft.head_share`
    -- and the ratio between a low laid-over leaf mass and a few standing
    stems. Where that loses a real field mark, the note says which one.

BIRD'S-FOOT TREFOIL IS NOT HERE. It is on the grassland list this file was
written against and it already ships as `specs/birdsfoot-trefoil.json` from
`tools/seed_wildflowers.py`. Re-authoring it would be a revert with extra
steps; `tools/seedspec.py` would refuse it anyway.

THE ONE MOSS RIDES ALONG. `moss-cushion` is `kind: grass` rather than `flower`
and belongs to the ground-cover list, but it is the same tuft generator with a
different head setting, and there is no `seed_groundcover2.py` for it to live
in. The grassland file's other ground-cover row -- sphagnum on the temperate
forest list -- is NOT authored here: `sphagnum-hummock` already ships and
already carries `biomes.temperate_forest` 0.5, so that row is covered and a
second sphagnum spec would be a duplicate species, not a new one.

SPACING NOW MEANS WHAT IT SAYS. `placement.spacing_m`'s floor came down from
0.5 m to 0.1 m (owner, 2026-08-15) because fifteen carpet species had been
silently clamped by it. Three species here are genuinely denser than half a
metre -- `bugle` at 0.3, `devils-thorn` at 0.4 and `moss-cushion` at 0.35 --
and each says in its notes what that number is measured against, which is the
width of one plant rather than a preference.

AUTHORED AT 5 cm, the detail lattice every flower and ground-cover spec uses.
Nothing here departs from it: a flower is not a terrain-lattice asset, and 5 cm
is where `tools/lattice_ab.py` measured a bloom surviving at all.

`tuft.base_m` is never smaller than `tuft.spread_m` on anything here. The root
crown is what makes a clump ONE PIECE at 26-connectivity, which is what
`tools/buildcheck.py` enforces and the most likely way a new tuft spec fails.

    python tools/seed_wildflowers2.py
    python tools/seed_wildflowers2.py --force    # revert them all to these drafts

SIZES ARE APPROXIMATE AND SAY SO. Every height below is the approximate figure
from the biome file it came from, and `docs/biomes/README.md` §8 is explicit
that those are unsourced general-knowledge estimates. Nothing here is quoted as
measured.
"""
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
import seedspec
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"


def base(kind="flower", **over):
    changes = {
        "kind": kind,
        "resolution_cm": "5",
        # Ground cover is placed in hundreds and it is cheap, so it can afford
        # to vary widely; a meadow of identical plants is the most obvious tell
        # that something is generated.
        "variation.amount": 1.0,
        "variation.height": 0.26,
        "variation.shape": 0.18,
        "variation.proportion": 0.20,
    }
    changes.update(over)
    return changes


def t(**kw):
    """`tuft.*`, `materials.*`, `biomes.*` and `placement.*` from keywords.

    Same translation `tools/seed_wildflowers.py` uses: `mat_stem=` becomes
    `materials.stem`, `bio_grassland=` becomes `biomes.grassland`,
    `place_abundance=` becomes `placement.abundance`, and everything else takes
    the `tuft.` prefix.
    """
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


# The paragraph seven specs below share, written once. Every one of them is a
# plant whose flower is under a voxel at 5 cm, and the biome files' own ⚠ notes
# say what to do about it.
OVERSIZE = ("THE BLOOM IS DRAWN AT SEVERAL TIMES LIFE SIZE AND THAT IS "
            "DELIBERATE. At 5 cm this species' real flower is well under one "
            "voxel, so it is drawn as a single voxel of a distinct colour the "
            "way `meadow-daisy` has since the library began -- which is house "
            "practice, is what the biome file's own ⚠ note asks for, and does "
            "read. DO NOT 'correct' it to the listed size.\n\n")


# name -> (blurb, changes)
SPECIES = {
    # --- grassland: 28.06% of land -------------------------------------------
    "harebell": (
        "0.35 m — the thinnest silhouette in the library, with pale blue bells",
        base(name="harebell",
             height_m=0.35,
             notes=OVERSIZE +
                   "THE GRASSLAND FILE REASONS THIS ONE OUT AND THIS SPEC "
                   "FOLLOWS IT. Its ⚠ says: stem 1-2 mm, flower 15 mm, on a "
                   "5 cm lattice, so either author the bloom oversize as one "
                   "voxel of a distinct colour on a one-voxel stem, or leave "
                   "the species out. It is authored, because 'a wildflower "
                   "meadow with no harebells' is a decision and this is the "
                   "other one.\n\n"
                   "WHAT SURVIVES IS THE THINNESS, not the bell. Four wiry "
                   "stems, the narrowest root spread in this file, a hard "
                   "taper and a high wander, so the plant is almost nothing: a "
                   "few pale blue dots that appear to float over grass with no "
                   "visible plant under them. `skin_blue` is the engine's royal "
                   "blue and a harebell is paler; the hue family is right and "
                   "the saturation is not.",
             **t(stems=4, spread_m=0.04, splay_deg=15, arc=0.36, width_m=0.05,
                 taper=0.85, wander=0.50, length_var=0.42, base_m=0.05,
                 head="bloom", head_m=0.07, head_share=0.6,
                 mat_stem="grass", mat_head="skin_blue",
                 bio_grassland=1.0, bio_temperate_forest=0.3,
                 bio_tundra_alpine=0.3, bio_taiga=0.2,
                 place_abundance=0.45, place_spacing_m=0.8, place_cluster=0.75,
                 place_elev_max_m=1400, place_slope_max_pct=55)),
    ),
    "prairie-coneflower": (
        "1.00 m — a tall bare wand with a long dark cone on the end",
        base(name="prairie-coneflower",
             height_m=1.00,
             notes="THE CONE IS DRAWN AND THE PETALS ARE NOT, which is the "
                   "opposite choice from `purple-coneflower` beside it and is "
                   "why the two are separate species rather than one at two "
                   "colours. A prairie coneflower's few yellow petals hang "
                   "straight DOWN off the rim, and a hanging petal 2 cm wide is "
                   "under half a voxel; the long dark cylinder above them is "
                   "three or four voxels and is what carries at any distance. "
                   "So the head is a `spike` in the brown-orange rather than a "
                   "`bloom` in yellow, and the skirt of petals is lost. There "
                   "is one head material, so it cannot be both.\n\n"
                   "Very few stems, nearly bare, low arc: a stand of these is "
                   "a scatter of dark thimbles held at head height over the "
                   "grass, with almost no plant visible beneath them.",
             **t(stems=5, spread_m=0.06, splay_deg=13, arc=0.16, width_m=0.05,
                 taper=0.7, wander=0.24, length_var=0.40, base_m=0.06,
                 head="spike", head_m=0.09, head_frac=0.16, head_share=0.7,
                 mat_stem="leaf_dry", mat_head="leaf_autumn",
                 bio_grassland=1.0, bio_savanna=0.3, bio_desert=0.2,
                 place_abundance=0.4, place_spacing_m=1.2, place_cluster=0.65,
                 place_slope_max_pct=45)),
    ),
    "common-milkweed": (
        "1.40 m — a thick upright stem with big paired leaves and pink domes",
        base(name="common-milkweed",
             height_m=1.40,
             notes="THE THICKEST STEM ON THE FLOWER PAGE, and that is the "
                   "species at ten voxels: a milkweed is a single fat vertical "
                   "with big paired leaves standing straight off it, not a "
                   "spray of wiry stalks. `width_m` 0.075 against the page's "
                   "usual 0.05, on very few stems, with a low splay and a low "
                   "arc so nothing leans.\n\n"
                   "The dusty-pink umbels are domes sitting IN the leaves "
                   "rather than held clear, so `head_share` is low and the "
                   "head is wide. The spindle-shaped seed pods that name it are "
                   "a later season and are not drawn -- there is one head "
                   "primitive and the flower is the more recognisable of the "
                   "two.",
             **t(stems=7, spread_m=0.08, splay_deg=18, arc=0.24, width_m=0.075,
                 taper=0.55, wander=0.22, length_var=0.30, base_m=0.08,
                 head="bloom", head_m=0.15, head_share=0.4,
                 mat_stem="leaf_broadleaf", mat_head="leaf_blossom",
                 bio_grassland=1.0, bio_temperate_forest=0.3, bio_savanna=0.2,
                 place_abundance=0.4, place_spacing_m=1.3, place_cluster=0.7,
                 place_elev_max_m=1200)),
    ),
    # --- temperate forest floor: the spring flora ----------------------------
    "wood-sorrel": (
        "0.22 m — a low sheet of folded three-part leaves with white stars",
        base(name="wood-sorrel",
             height_m=0.22,
             notes=OVERSIZE +
                   "AUTHORED AT 0.22 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.10 m, which is the library's floor for a thing that has "
                   "to be drawn at all: at 5 cm 0.10 m is two voxels and there "
                   "is no room for a leaf and a flower in the same plant. The "
                   "temperate-forest file's ⚠ names this species and the dog "
                   "violet together and asks for exactly this treatment.\n\n"
                   "IT IS A SHEET, NOT A PLANT. Wood sorrel covers whole banks "
                   "of a wood in shade, so it is authored to tile -- 0.4 m "
                   "spacing at near-maximum clustering -- and drawn as a dense "
                   "low mass of laid-over broad stems with white flecks just "
                   "clear of it. The three heart-shaped leaflets that name it "
                   "are one voxel between them and are not attempted.",
             **t(stems=16, spread_m=0.11, splay_deg=54, arc=0.80, width_m=0.075,
                 taper=0.6, wander=0.45, length_var=0.25, base_m=0.11,
                 head="bloom", head_m=0.07, head_share=0.35,
                 mat_stem="leaf_jungle", mat_head="plume_white",
                 bio_temperate_forest=1.0, bio_taiga=0.4,
                 bio_rainforest=0.2,
                 place_abundance=0.85, place_spacing_m=0.4, place_cluster=0.95,
                 place_water_max_m=60, place_elev_max_m=900)),
    ),
    "lily-of-the-valley": (
        "0.25 m — two broad upright leaves with one arched stem of bells",
        base(name="lily-of-the-valley",
             height_m=0.25,
             notes="TWO LEAVES AND ONE ARCH, and the generator has one arc for "
                   "the whole clump. In life the leaves stand and only the "
                   "flowering stem bends over behind them; here every stem "
                   "shares `tuft.arc`, so the compromise is a middling arc on "
                   "very few, very broad stems -- the leaves lean a little more "
                   "than they should and the bell stem a little less. That is "
                   "the one real loss in this spec and it is recorded rather "
                   "than tuned around.\n\n"
                   "What does carry: the count. Three or four stems is the "
                   "whole plant, the widest stems on the page at 0.10 m, and "
                   "one narrow one in three carrying a short one-sided run of "
                   "white bells. It carpets under beech, so the spacing is "
                   "0.45 m -- below the old 0.5 floor and meant, because one "
                   "plant is two leaves wide and a colony is continuous ground "
                   "cover with no soil showing.",
             **t(stems=4, spread_m=0.06, splay_deg=20, arc=0.42, width_m=0.10,
                 taper=0.45, wander=0.20, length_var=0.22, base_m=0.06,
                 head="spike", head_m=0.07, head_frac=0.32, head_share=0.3,
                 mat_stem="leaf_broadleaf", mat_head="plume_white",
                 bio_temperate_forest=1.0, bio_taiga=0.3,
                 place_abundance=0.7, place_spacing_m=0.45, place_cluster=0.95,
                 place_elev_max_m=900)),
    ),
    "common-dog-violet": (
        "0.22 m — a flat heart-leaf mat with violet flecks just above it",
        base(name="common-dog-violet",
             height_m=0.22,
             notes=OVERSIZE +
                   "AUTHORED AT 0.22 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.12 m, the same five-voxel floor `wood-sorrel` takes and "
                   "for the same ⚠.\n\n"
                   "THE FLOWER IS HELD JUST ABOVE THE LEAVES AND THAT GAP IS "
                   "THE SPECIES -- a violet is not a mat with colour in it, it "
                   "is a mat with colour a centimetre over it. At this lattice "
                   "that gap is one voxel, and it is bought by arcing the leaf "
                   "stems harder than the flowering ones can reach: a high arc "
                   "on a low head share, so the plain stems lie right over and "
                   "the few heads sit clear. Against `wood-sorrel` beside it "
                   "the difference is that gap plus the colour; the two share a "
                   "wood and must not read as one plant.",
             **t(stems=14, spread_m=0.10, splay_deg=56, arc=0.84, width_m=0.07,
                 taper=0.55, wander=0.45, length_var=0.30, base_m=0.10,
                 head="bloom", head_m=0.07, head_share=0.3,
                 mat_stem="leaf_broadleaf", mat_head="plume_lilac",
                 bio_temperate_forest=1.0, bio_grassland=0.4, bio_taiga=0.25,
                 place_abundance=0.7, place_spacing_m=0.5, place_cluster=0.9,
                 place_elev_max_m=1000)),
    ),
    "herb-robert": (
        "0.35 m — a sprawling red-stemmed plant with hard pink flowers",
        base(name="herb-robert",
             height_m=0.35,
             notes="THE RED STEM IS THE SPECIES AND THE STEM PALETTE HAS NO "
                   "RED. `materials.stem` offers seven choices and six are "
                   "greens, a straw and a savanna tan; `podzol` is the seventh "
                   "and it is a red-brown soil colour, which is the nearest "
                   "thing available and is duller and browner than the plant. A "
                   "red or purple STEM material is a one-row choice-list ask "
                   "and this species is the argument for it -- the same shape "
                   "of ask `snowberry` records for a white leaf.\n\n"
                   "Sprawling rather than upright: high splay, high arc, many "
                   "fine stems out of a wide crown, so it lies over rock and "
                   "wall bases instead of standing. The finely cut leaves are "
                   "sub-voxel and are not attempted; the small hard pink "
                   "flowers are, and they sit on the sprawl.",
             **t(stems=16, spread_m=0.11, splay_deg=50, arc=0.72, width_m=0.05,
                 taper=0.65, wander=0.50, length_var=0.35, base_m=0.11,
                 head="bloom", head_m=0.08, head_share=0.45,
                 mat_stem="podzol", mat_head="leaf_blossom",
                 bio_temperate_forest=1.0, bio_grassland=0.4,
                 bio_bare_rock=0.2,
                 place_abundance=0.6, place_spacing_m=0.7, place_cluster=0.8,
                 place_slope_max_pct=60)),
    ),
    "trout-lily": (
        "0.24 m — two mottled leaves and one nodding yellow flower",
        base(name="trout-lily",
             height_m=0.24,
             notes="AUTHORED AT 0.24 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.20 m, to clear the five-voxel floor with a head on top. "
                   "Written down so it is not shrunk back.\n\n"
                   "THREE STEMS IS THE WHOLE PLANT: two broad plain leaves and "
                   "one narrower one carrying a single nodding yellow flower. "
                   "That count IS the shape, the same way it is for "
                   "`large-trillium` -- add stems and it becomes a generic "
                   "clump. `length_var` is the lowest here because the two "
                   "leaves are a matched pair.\n\n"
                   "THE MOTTLING IS THE NAME AND IT IS NOT DRAWN. Brown-green "
                   "marbling on a leaf two voxels wide has nowhere to go, and "
                   "there is one stem material. What identifies it here is the "
                   "count, the nod and the yellow.",
             **t(stems=3, spread_m=0.05, splay_deg=34, arc=0.52, width_m=0.09,
                 taper=0.5, wander=0.22, length_var=0.16, base_m=0.06,
                 head="bloom", head_m=0.09, head_share=0.35,
                 mat_stem="leaf_broadleaf", mat_head="skin_yellow",
                 bio_temperate_forest=1.0, bio_taiga=0.2,
                 place_abundance=0.5, place_spacing_m=0.6, place_cluster=0.9,
                 place_elev_max_m=1000)),
    ),
    "hellebore": (
        "0.50 m — coarse dark palmate leaves under nodding cream cups",
        base(name="hellebore",
             height_m=0.50,
             notes="THE FLOWERS HANG UNDER THE LEAVES, which is the reverse of "
                   "everything else on this page and is what a winter-flowering "
                   "plant looks like: a coarse dark evergreen mound with pale "
                   "cups nodding out from beneath its own foliage. It is "
                   "authored as broad stiff stems arched right over on a wide "
                   "crown, with a small share of shorter stems carrying the "
                   "head -- so the heads end up low and inside the mass rather "
                   "than above it.\n\n"
                   "`plume_buff` for the head, which is the palette's cream. "
                   "The real flower is green-cream and there is no green-white "
                   "in the head menu; buff is the nearer half of it.",
             **t(stems=10, spread_m=0.10, splay_deg=40, arc=0.66, width_m=0.09,
                 taper=0.5, wander=0.28, length_var=0.40, base_m=0.10,
                 head="bloom", head_m=0.11, head_share=0.3,
                 mat_stem="leaf_needle", mat_head="plume_buff",
                 bio_temperate_forest=1.0, bio_grassland=0.2,
                 place_abundance=0.35, place_spacing_m=1.0, place_cluster=0.7,
                 place_elev_max_m=1200)),
    ),
    "cyclamen": (
        "0.22 m — round leaves flat on the ground, pink flowers on bare stalks",
        base(name="cyclamen",
             height_m=0.22,
             notes=OVERSIZE +
                   "AUTHORED AT 0.22 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.15 m, for the five-voxel floor.\n\n"
                   "THE SAME STRUCTURE AS `cowslip` AND A DIFFERENT PLANT, "
                   "which is worth saying because at this size the two are "
                   "built from the same three facts: leaves pressed flat, a few "
                   "bare stalks standing out of the middle, one flower each. "
                   "What separates them is proportion -- a cyclamen's head is "
                   "the smallest bloom on the page against the cowslip's "
                   "cluster, its leaves are round rather than crinkled lances, "
                   "and it flowers in autumn under trees rather than in spring "
                   "grass.\n\n"
                   "The upswept petals -- the one thing that makes a cyclamen "
                   "unmistakable in life -- are a 1 cm gesture and are not "
                   "drawn. The marbled leaf is not drawn either; there is one "
                   "stem material.",
             **t(stems=12, spread_m=0.10, splay_deg=58, arc=0.82, width_m=0.08,
                 taper=0.6, wander=0.35, length_var=0.40, base_m=0.10,
                 head="bloom", head_m=0.07, head_share=0.25,
                 mat_stem="leaf_broadleaf", mat_head="leaf_blossom",
                 bio_temperate_forest=1.0, bio_grassland=0.2,
                 place_abundance=0.5, place_spacing_m=0.6, place_cluster=0.9,
                 place_elev_max_m=1200)),
    ),
    "bugle": (
        "0.25 m — a creeping bronze mat with short dense blue spikes",
        base(name="bugle",
             height_m=0.25,
             notes="A MAT WITH VERTICALS OUT OF IT, and the two halves are "
                   "authored as two halves of `tuft.head_share`: three quarters "
                   "of the stems lie right over as the creeping bronze-purple "
                   "mat, and a quarter stand up carrying a short dense blue "
                   "spike. Nothing else on this page has that contrast at this "
                   "height -- the other mats here put their flowers ON the mat "
                   "and this one holds them off it.\n\n"
                   "`podzol` for the leaves, which is the only brown in the "
                   "stem palette and is the nearest thing to bugle's "
                   "bronze-purple foliage; `skin_blue` for the spike.\n\n"
                   "SPACING 0.3 m, WHICH IS BELOW THE OLD 0.5 FLOOR AND IS "
                   "MEANT. Bugle runs on surface stolons and roots where it "
                   "touches, so one plant is a hand's width across and a colony "
                   "is continuous. The floor came down to 0.1 m on 2026-08-15 "
                   "precisely so a carpet could say so.",
             **t(stems=14, spread_m=0.11, splay_deg=48, arc=0.74, width_m=0.06,
                 taper=0.55, wander=0.40, length_var=0.35, base_m=0.11,
                 head="spike", head_m=0.08, head_frac=0.34, head_share=0.28,
                 mat_stem="podzol", mat_head="skin_blue",
                 bio_temperate_forest=1.0, bio_grassland=0.5, bio_taiga=0.2,
                 place_abundance=0.75, place_spacing_m=0.3, place_cluster=0.95,
                 place_water_max_m=80, place_elev_max_m=900)),
    ),
    # --- beach: it wraps every coastline in the world -------------------------
    "beach-morning-glory": (
        "0.22 m — long runners over sand with fleshy round leaves and purple trumpets",
        base(name="beach-morning-glory",
             height_m=0.22,
             notes="THE TROPICAL COUNTERPART TO `sea-bindweed`, and deliberately "
                   "the same build with three things moved: the leaves are "
                   "bigger and fleshier (`width_m` 0.08 against 0.05), the "
                   "flower is purple rather than pink-striped, and it belongs "
                   "on hot shores. The pair is worth having because between "
                   "them they cover the same job on a temperate and a tropical "
                   "beach, and a scatterer picking by biome gets the right "
                   "one.\n\n"
                   "The flattest habit the generator will draw -- the widest "
                   "root spread and the highest splay on this page -- because "
                   "the plant is runners rather than stems and only the "
                   "trumpets stand. The runners themselves reach several metres "
                   "in life and no single asset can be several metres of "
                   "creeper; what is authored is one rosette of it, and the "
                   "length is a placement matter.",
             **t(stems=18, spread_m=0.16, splay_deg=66, arc=0.90, width_m=0.08,
                 taper=0.7, wander=0.60, length_var=0.30, base_m=0.16,
                 head="bloom", head_m=0.10, head_share=0.35,
                 mat_stem="leaf_jungle", mat_head="plume_lilac",
                 bio_beach=1.0, bio_rainforest=0.2,
                 place_abundance=0.45, place_spacing_m=1.0, place_cluster=0.8,
                 place_elev_max_m=10, place_slope_max_pct=35)),
    ),
    "sea-aster": (
        "0.50 m — fleshy narrow leaves and pale mauve daisies on tidal mud",
        base(name="sea-aster",
             height_m=0.50,
             notes="A DAISY ON A SUCCULENT, which is the odd combination that "
                   "identifies it: the flower is an ordinary flat mauve disc "
                   "and the plant under it is fleshy and blue-green like the "
                   "glasswort it grows beside. So the head is a plain wide "
                   "`bloom` and the work is in the stems -- thick, barely "
                   "tapered, few, stiff.\n\n"
                   "Salt marsh and tidal mud only, which is a placement "
                   "statement: hard against water, nearly flat ground, and the "
                   "lowest elevation ceiling on the page after `sea-rocket`.",
             **t(stems=10, spread_m=0.09, splay_deg=26, arc=0.34, width_m=0.065,
                 taper=0.8, wander=0.28, length_var=0.35, base_m=0.09,
                 head="bloom", head_m=0.12, head_share=0.5,
                 mat_stem="leaf_jungle", mat_head="plume_lilac",
                 bio_beach=1.0,
                 place_abundance=0.55, place_spacing_m=0.6, place_cluster=0.9,
                 place_water_max_m=10, place_elev_max_m=6,
                 place_slope_max_pct=10)),
    ),
    # --- savanna: 20.76% of land ---------------------------------------------
    "devils-thorn": (
        "0.22 m — a flat radiating mat on bare ground with yellow flecks in it",
        base(name="devils-thorn",
             height_m=0.22,
             notes=OVERSIZE +
                   "AUTHORED AT 0.22 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.10 m. The savanna file's ⚠ is explicit: 10 cm on the 5 cm "
                   "lattice is two voxels and the flower is smaller than one, "
                   "so it cannot be an individual plant -- author it as a low "
                   "mat with colour flecks or leave it to the ground material. "
                   "This is the mat.\n\n"
                   "THE FLATTEST THING IN THE LIBRARY, and it has to be: a "
                   "devil's thorn runs its paired leaflets along the ground in "
                   "a radiating star with no height at all. Maximum splay, "
                   "maximum arc, the widest root spread on the page, and the "
                   "yellow flowers sit directly ON the mat rather than over it "
                   "-- so the asset reads as a patch of patterned ground with "
                   "colour in it, which is what it is.\n\n"
                   "SPACING 0.4 m, BELOW THE OLD 0.5 FLOOR AND MEANT: one mat "
                   "is a hand-span to a foot across and they meet edge to edge "
                   "on trampled bare ground, which is exactly where this plant "
                   "lives.",
             **t(stems=20, spread_m=0.17, splay_deg=70, arc=0.92, width_m=0.06,
                 taper=0.7, wander=0.55, length_var=0.22, base_m=0.17,
                 head="bloom", head_m=0.07, head_share=0.5,
                 mat_stem="grass", mat_head="skin_yellow",
                 bio_savanna=1.0, bio_desert=0.5, bio_grassland=0.3,
                 place_abundance=0.55, place_spacing_m=0.4, place_cluster=0.85,
                 place_slope_max_pct=25)),
    ),
    # --- ground cover: one moss, riding along on the same generator ----------
    "moss-cushion": (
        "grass 0.22 m — a tight bright green dome in a hollow or a stone's lee",
        base("grass", name="moss-cushion",
             height_m=0.22,
             notes="AUTHORED AT 0.22 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.06 m, which is the same departure `feather-moss` records "
                   "and carries the same open question: at 5 cm a real moss "
                   "cushion is ONE VOXEL, and a one-voxel mat is not an object "
                   "-- it is the top of the ground, and the honest "
                   "implementation may be a terrain surface material. This spec "
                   "does not close that question. It is authored because a "
                   "grassland with no moss in its hollows is bare in exactly "
                   "the places that should not be.\n\n"
                   "A CUSHION AND NOT A CARPET, which is what separates it from "
                   "`feather-moss` and `woolly-fringe-moss`: those two tile to "
                   "cover ground, and a moss cushion is a discrete dome sitting "
                   "in a hollow or in a stone's lee with bare ground around it. "
                   "The shape is the highest stem count in the flower file on a "
                   "narrow crown with a steep splay and a hard arc; the "
                   "distinction from the carpets lives in placement, not in "
                   "geometry.\n\n"
                   "SPACING 0.35 m, BELOW THE OLD 0.5 FLOOR AND MEANT: one "
                   "cushion is 10-20 cm across, so 0.35 m is two cushion widths "
                   "and lets a group of them gather in one hollow without "
                   "fusing into a sheet. Clustering is high and abundance is "
                   "middling, which is how a scatterer produces patches rather "
                   "than a lawn.",
             **t(stems=36, spread_m=0.08, splay_deg=52, arc=0.88, width_m=0.06,
                 taper=0.78, wander=0.55, length_var=0.18, base_m=0.08,
                 head="none", mat_stem="leaf_jungle",
                 bio_grassland=0.9, bio_temperate_forest=0.6, bio_taiga=0.5,
                 bio_tundra_alpine=0.4, bio_bare_rock=0.25,
                 place_abundance=0.6, place_spacing_m=0.35, place_cluster=0.9,
                 place_slope_max_pct=60)),
    ),
}


def main() -> int:
    force = seedspec.parse_force(sys.argv[1:])
    seedspec.announce(force, "wildflower specs")
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

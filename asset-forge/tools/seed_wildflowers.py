"""Author the wildflower set: forty-seven species off the shipped tuft generator.

WHY THIS IS FIRST. `docs/biomes/README.md` §7 puts grassland wildflowers at the
top of the whole build queue, and the arithmetic behind that is not close:
grassland is 28.06% of the world's land and had TWO flower specs, savanna is
20.76% and had one, and every one of these is a tuft -- a generator that has
been shipped and proven since the library began. Nothing else in the biome
files costs so little per unit of visible change, because a flower spec changes
the colour of a whole hillside rather than adding one object to it.

AUTHORED AT 5 cm, which is the detail lattice ground cover already uses
(`forge/kinds.py:44-51`). A flower is not a terrain-lattice asset, so it is
free to be finer than 10 cm, and 5 cm is where `tools/lattice_ab.py` measured
the bloom surviving: at 10 cm a 12 cm daisy head is one voxel.

THE SIZE FLOOR, AND WHO IT BINDS. Nothing here is authored under 0.22 m, which
at 5 cm is four or five voxels of stem plus a head. Seven species are genuinely
smaller than that in life -- wild thyme, wood anemone, purple saxifrage,
mountain avens, trumpet gentian, sea bindweed, sand verbena -- and every one of
them says so in its own `notes`, because a note is the only thing standing
between the next reader and a well-meant "correction" back to the size in the
biome table. That is the same fix `clown-anemonefish` and four birds already
carry.

THE BLOOM PALETTE WAS THE REAL CONSTRAINT AND IT HAS BEEN WIDENED. Before this
pass `materials.head` offered seven choices and six were foliage: a pale cherry
pink, a brown-orange, a straw yellow, two greens, a tan and snow. A poppy, a
cornflower and a knapweed all came out the same pink. `forge/spec.py` now also
offers seven creature materials that are ALREADY IN THE ENGINE -- true white,
crimson, lilac, buff, blue, yellow and orange, ids the fish and bird appends
put there -- so this is a wider menu and not a material append, and
`forge.cli selftest`'s "materials exist in the engine" check still passes.
Blue and violet in particular did not exist at all before, and between them
they are most of a European meadow.

WHAT A FLOWER IS, IN THIS GENERATOR. A few stems from a common root disc, some
of them carrying a head and the rest left plain -- which is how the plant gets
its leaves for nothing (`tuft.head_share`). Three shapes recur below and they
are worth naming because they are what separates one species from another at
eight voxels:

  * A DISC on a bare stalk -- daisy, marigold, gerbera. One head per stem, held
    at a common height, so the flowers read as a floating coloured layer over a
    leaf mass. Low `arc`, high `head_share`, small `head_m`.
  * A SPIKE -- foxglove, fireweed, lupine, blazing star, viper's bugloss. One
    or two stems, near-vertical, and the head is a long share of the stem
    rather than a knob on the end. `tuft.head` is `spike` and `head_frac` does
    the work.
  * A MAT -- thyme, saxifrage, avens, sea bindweed. Wide root spread, high
    splay, high arc, so the stems lie over rather than stand, and the flowers
    sit ON the mat. These are the ones authored above life size.

`tuft.base_m` is never smaller than `tuft.spread_m` on anything here. The root
crown is what makes a clump ONE PIECE at 26-connectivity, which is what
`tools/buildcheck.py` enforces and the most likely way a new tuft spec fails.

    python tools/seed_wildflowers.py
    python tools/seed_wildflowers.py --force    # revert them all to these drafts

SIZES ARE APPROXIMATE AND SAY SO. Every height below is the approximate figure
from the biome file it came from, and `docs/biomes/README.md` §8 is explicit
that those are unsourced general-knowledge estimates. They are good enough to
choose a lattice and to size one plant against its neighbour; nothing here is
quoted as measured, because this project has already shipped one fabricated
citation and the defence is refusing to state precision that does not exist.
"""
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
import seedspec
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"


def base(**over):
    changes = {
        "kind": "flower",
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

    `mat_stem=` becomes `materials.stem`, `bio_grassland=` becomes
    `biomes.grassland`, `place_abundance=` becomes `placement.abundance`, and
    everything else takes the `tuft.` prefix. Python keywords cannot contain a
    dot; forty species times fifteen rows is enough repetition to pay for the
    translation.
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


# name -> (blurb, changes)
SPECIES = {
    # --- grassland: 28.06% of land, and it had two flower specs -------------
    "common-poppy": (
        "0.60 m — wiry stems, one wide scarlet cup each",
        base(name="common-poppy",
             height_m=0.60,
             notes="THE SPECIES THE CRIMSON BLOOM COLOUR EXISTS FOR. A poppy "
                   "is one flat scarlet cup on a wiry hairy stem, and before "
                   "the head palette was widened it came out the same pale "
                   "pink as a daisy. Few stems, well spaced, blooms held clear "
                   "of the leaves -- the plant reads as scattered red dots over "
                   "grass rather than as a clump.",
             **t(stems=7, spread_m=0.07, splay_deg=22, arc=0.24, width_m=0.05,
                 taper=0.75, wander=0.30, length_var=0.40, base_m=0.07,
                 head="bloom", head_m=0.14, head_share=0.55,
                 mat_stem="grass", mat_head="plume_crimson",
                 bio_grassland=1.0, bio_temperate_forest=0.3,
                 bio_savanna=0.25,
                 place_abundance=0.55, place_spacing_m=0.9, place_cluster=0.75,
                 place_elev_max_m=1200, place_slope_max_pct=45)),
    ),
    "cornflower": (
        "0.70 m — slim grey-green stems, one ragged blue head each",
        base(name="cornflower",
             height_m=0.70,
             notes="The first genuinely BLUE plant in the library. `skin_blue` "
                   "is the engine's royal blue -- appended for reef fish -- and "
                   "it is the nearest thing the palette has; nothing else here "
                   "is even in the right hue family. Tall, slim, very few "
                   "stems, one head each, so it stands above the grass rather "
                   "than in it.",
             **t(stems=6, spread_m=0.06, splay_deg=16, arc=0.18, width_m=0.05,
                 taper=0.7, wander=0.25, length_var=0.35, base_m=0.06,
                 head="bloom", head_m=0.12, head_share=0.6,
                 mat_stem="leaf_dry", mat_head="skin_blue",
                 bio_grassland=1.0, bio_savanna=0.2,
                 place_abundance=0.4, place_spacing_m=1.1, place_cluster=0.7,
                 place_elev_max_m=1000)),
    ),
    "oxeye-daisy": (
        "0.70 m — the big white daisy: one head per stem, held level",
        base(name="oxeye-daisy",
             height_m=0.70,
             notes="`meadow-daisy` at twice the height with a true white head "
                   "instead of a blossom pink. The two are deliberately "
                   "different species rather than one at two sizes: this one "
                   "carries one head per stem at a common height, which is what "
                   "makes an oxeye meadow read as a white LAYER floating over "
                   "green.",
             **t(stems=9, spread_m=0.08, splay_deg=18, arc=0.22, width_m=0.05,
                 taper=0.75, wander=0.28, length_var=0.30, base_m=0.08,
                 head="bloom", head_m=0.16, head_share=0.7,
                 mat_stem="grass", mat_head="plume_white",
                 bio_grassland=1.0, bio_temperate_forest=0.45,
                 bio_taiga=0.2,
                 place_abundance=0.7, place_spacing_m=0.8, place_cluster=0.8,
                 place_elev_max_m=1600)),
    ),
    "yarrow": (
        "0.60 m — flat white plate over feathery leaves",
        base(name="yarrow",
             height_m=0.60,
             notes="A FLAT-TOPPED PLATE, not a disc on a stalk: the head is "
                   "wide and the stems are stiff and near-vertical so every "
                   "plate lands at the same height. Wide `head_m` on a low "
                   "`arc` is the whole trick, and it is the one silhouette that "
                   "separates an umbel from a daisy at five voxels.",
             **t(stems=8, spread_m=0.07, splay_deg=12, arc=0.14, width_m=0.05,
                 taper=0.8, wander=0.22, length_var=0.25, base_m=0.07,
                 head="bloom", head_m=0.20, head_share=0.65,
                 mat_stem="leaf_needle", mat_head="plume_white",
                 bio_grassland=1.0, bio_temperate_forest=0.35,
                 bio_beach=0.3, bio_taiga=0.25,
                 place_abundance=0.6, place_spacing_m=0.9, place_cluster=0.7)),
    ),
    "common-knapweed": (
        "0.80 m — stiff branched stems, purple thistle brushes",
        base(name="common-knapweed",
             height_m=0.80,
             notes="Violet, which the palette had none of until `plume_lilac` "
                   "was opened to the tuft kinds. A hard scaly bud opening to a "
                   "brush, so the head is small and dense rather than wide and "
                   "flat -- the opposite end of `head_m` from the yarrow beside "
                   "it, on a taller stiffer stem.",
             **t(stems=7, spread_m=0.07, splay_deg=20, arc=0.20, width_m=0.055,
                 taper=0.65, wander=0.30, length_var=0.35, base_m=0.07,
                 head="bloom", head_m=0.11, head_share=0.5,
                 mat_stem="leaf_dry", mat_head="plume_lilac",
                 bio_grassland=1.0, bio_temperate_forest=0.3,
                 place_abundance=0.5, place_spacing_m=1.0, place_cluster=0.65)),
    ),
    "field-scabious": (
        "0.80 m — long bare stems, flat lilac pincushions",
        base(name="field-scabious",
             height_m=0.80,
             notes="Nearly all bare stem with one flat pincushion on top, which "
                   "is why `head_share` is low and the leaves are the plain "
                   "stems. It reads as knapweed's opposite at the same height: "
                   "a wide flat head on a bare wand rather than a tight brush "
                   "on a branched one.",
             **t(stems=6, spread_m=0.07, splay_deg=18, arc=0.26, width_m=0.05,
                 taper=0.8, wander=0.32, length_var=0.40, base_m=0.07,
                 head="bloom", head_m=0.15, head_share=0.45,
                 mat_stem="grass", mat_head="plume_lilac",
                 bio_grassland=0.9, bio_temperate_forest=0.3,
                 place_abundance=0.4, place_spacing_m=1.2, place_cluster=0.6)),
    ),
    "red-clover": (
        "0.40 m — low leafy mound with dense round pink heads",
        base(name="red-clover",
             height_m=0.40,
             notes="A LEAF plant that happens to flower, which is the reverse "
                   "of everything else on this page: most stems are plain and "
                   "arched over, and only a third carry a head. That is what "
                   "makes clover read as a soft mat with dots in it rather than "
                   "as a stand of flowers.",
             **t(stems=14, spread_m=0.10, splay_deg=34, arc=0.55, width_m=0.05,
                 taper=0.6, wander=0.45, length_var=0.35, base_m=0.10,
                 head="bloom", head_m=0.10, head_share=0.35,
                 mat_stem="leaf_broadleaf", mat_head="leaf_blossom",
                 bio_grassland=1.0, bio_temperate_forest=0.4,
                 bio_beach=0.25,
                 place_abundance=0.85, place_spacing_m=0.55, place_cluster=0.85)),
    ),
    "birdsfoot-trefoil": (
        "0.25 m — sprawling low stems with yellow-orange pea clusters",
        base(name="birdsfoot-trefoil",
             height_m=0.25,
             notes="The low sprawler. High splay and high arc lay the stems "
                   "over so the plant is wider than it is tall, and the heads "
                   "sit on the mat rather than above it. Saturated yellow, "
                   "which `leaf_dry` could not give -- that is a straw colour "
                   "for dead grass, and a trefoil next to it looks wilted.",
             **t(stems=16, spread_m=0.11, splay_deg=48, arc=0.75, width_m=0.05,
                 taper=0.6, wander=0.5, length_var=0.35, base_m=0.11,
                 head="bloom", head_m=0.09, head_share=0.45,
                 mat_stem="grass", mat_head="skin_yellow",
                 bio_grassland=1.0, bio_beach=0.4, bio_temperate_forest=0.25,
                 place_abundance=0.75, place_spacing_m=0.6, place_cluster=0.8)),
    ),
    "wild-thyme": (
        "0.24 m — creeping woody mat, dense pink-purple heads",
        base(name="wild-thyme",
             height_m=0.24,
             notes="AUTHORED AT 0.24 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.10 m, and the reason is the lattice rather than the "
                   "plant: at 5 cm a 10 cm mat is two voxels tall and there is "
                   "nothing to draw -- no stem, no head, no shape. 0.24 m is "
                   "five voxels, which is the floor at which a mat can carry a "
                   "bloom that is not the mat itself. This is the same "
                   "above-life-size fix `alpine-cushion-flower` already "
                   "carries; DO NOT 'correct' it back.\n\n"
                   "It is still authored as a MAT -- very wide root spread, "
                   "steep splay, stems laid right over -- so it reads low and "
                   "spreading rather than as a short upright plant.",
             **t(stems=20, spread_m=0.13, splay_deg=58, arc=0.85, width_m=0.05,
                 taper=0.7, wander=0.55, length_var=0.30, base_m=0.13,
                 head="bloom", head_m=0.08, head_share=0.6,
                 mat_stem="leaf_needle", mat_head="plume_lilac",
                 bio_grassland=0.9, bio_tundra_alpine=0.4, bio_beach=0.3,
                 place_abundance=0.6, place_spacing_m=0.7, place_cluster=0.9,
                 place_slope_max_pct=60)),
    ),
    "common-chicory": (
        "1.00 m — tall stiff near-leafless stems, sky-blue daisies",
        base(name="common-chicory",
             height_m=1.00,
             notes="The tallest flower here, and almost bare: the blooms are "
                   "pressed against a stiff branched stem rather than held out "
                   "on stalks, so `arc` is very low and `head_m` small. A "
                   "roadside and dry-verge plant, and the second species that "
                   "only exists because the head palette gained a blue.",
             **t(stems=5, spread_m=0.06, splay_deg=14, arc=0.12, width_m=0.055,
                 taper=0.6, wander=0.20, length_var=0.40, base_m=0.06,
                 head="bloom", head_m=0.10, head_share=0.7,
                 mat_stem="leaf_dry", mat_head="skin_blue",
                 bio_grassland=0.9, bio_savanna=0.3, bio_beach=0.2,
                 place_abundance=0.35, place_spacing_m=1.4, place_cluster=0.5)),
    ),
    "vipers-bugloss": (
        "0.70 m — bristly upright spike, blue funnels",
        base(name="vipers-bugloss",
             height_m=0.70,
             notes="A SPIKE rather than a disc: the head runs a third of the "
                   "way down the stem, which `tuft.head_frac` says and "
                   "`head_m` cannot. One or two stems only, because a bugloss "
                   "is a single bristly wand and a bunch of them reads as a "
                   "bush.",
             **t(stems=4, spread_m=0.05, splay_deg=12, arc=0.14, width_m=0.06,
                 taper=0.55, wander=0.20, length_var=0.30, base_m=0.05,
                 head="spike", head_m=0.13, head_frac=0.34, head_share=0.75,
                 mat_stem="leaf_needle", mat_head="skin_blue",
                 bio_grassland=0.8, bio_beach=0.45, bio_desert=0.2,
                 place_abundance=0.3, place_spacing_m=1.5, place_cluster=0.55)),
    ),
    "purple-coneflower": (
        "0.90 m — drooping mauve petals round a tall bristly cone",
        base(name="purple-coneflower",
             height_m=0.90,
             notes="THE CONE IS THE SPECIES, and at 5 cm a cone and a disc are "
                   "the same three voxels -- so what carries it here is the "
                   "proportion instead: a tall stiff stem, one head, and a head "
                   "wider than a daisy's on a plant twice its height. The "
                   "prairie counterpart to the knapweed.",
             **t(stems=6, spread_m=0.07, splay_deg=14, arc=0.16, width_m=0.06,
                 taper=0.6, wander=0.22, length_var=0.35, base_m=0.07,
                 head="bloom", head_m=0.17, head_share=0.6,
                 mat_stem="leaf_broadleaf", mat_head="plume_lilac",
                 bio_grassland=1.0, bio_savanna=0.3,
                 place_abundance=0.45, place_spacing_m=1.1, place_cluster=0.7)),
    ),
    "blazing-star": (
        "1.20 m — dense vertical bottlebrush of purple",
        base(name="blazing-star",
             height_m=1.20,
             notes="The purest SPIKE in the set: the head is more than half the "
                   "stem, which is the top of `tuft.head_frac`'s range, and the "
                   "plant is a single vertical bar of colour. Prairie, and the "
                   "one flower here tall enough to be seen over big bluestem.",
             **t(stems=4, spread_m=0.05, splay_deg=8, arc=0.08, width_m=0.06,
                 taper=0.5, wander=0.14, length_var=0.30, base_m=0.05,
                 head="spike", head_m=0.12, head_frac=0.50, head_share=0.8,
                 mat_stem="leaf_dry", mat_head="plume_lilac",
                 bio_grassland=1.0, bio_savanna=0.25,
                 place_abundance=0.3, place_spacing_m=1.6, place_cluster=0.75)),
    ),
    "prairie-lupine": (
        "0.90 m — bold vertical spike over a palmate leaf whorl",
        base(name="prairie-lupine",
             height_m=0.90,
             notes="A spike with LEAVES, which is the difference from the "
                   "blazing star beside it: half the stems carry no head at "
                   "all and arc out low, so the plant is a leaf whorl with "
                   "flower bars standing out of it. `tuft.head_share` is doing "
                   "the entire job.",
             **t(stems=9, spread_m=0.09, splay_deg=26, arc=0.40, width_m=0.06,
                 taper=0.6, wander=0.30, length_var=0.35, base_m=0.09,
                 head="spike", head_m=0.14, head_frac=0.40, head_share=0.45,
                 mat_stem="leaf_broadleaf", mat_head="plume_lilac",
                 bio_grassland=0.9, bio_temperate_forest=0.3,
                 bio_tundra_alpine=0.25,
                 place_abundance=0.4, place_spacing_m=1.2, place_cluster=0.8)),
    ),
    "california-poppy": (
        "0.35 m — low blue-grey mound with silky orange cups",
        base(name="california-poppy",
             height_m=0.35,
             notes="The orange one, and a genuinely different plant from the "
                   "scarlet poppy above: low, mounded, many fine stems, and the "
                   "flowers sit in the foliage rather than above it. `arc` "
                   "high, `head_m` small, `head_share` middling.",
             **t(stems=15, spread_m=0.09, splay_deg=36, arc=0.60, width_m=0.05,
                 taper=0.7, wander=0.45, length_var=0.35, base_m=0.09,
                 head="bloom", head_m=0.11, head_share=0.45,
                 mat_stem="leaf_needle", mat_head="skin_orange",
                 bio_grassland=0.8, bio_desert=0.5, bio_savanna=0.3,
                 place_abundance=0.5, place_spacing_m=0.8, place_cluster=0.85)),
    ),
    "cowslip": (
        "0.30 m — tight rosette, one stalk of nodding yellow tubes",
        base(name="cowslip",
             height_m=0.30,
             notes="A ROSETTE with one flowering stalk: nearly every stem is a "
                   "leaf laid right over, and one or two stand up carrying a "
                   "tight one-sided head. That contrast -- flat leaves and a "
                   "single vertical -- is the silhouette, and it is why "
                   "`head_share` is the lowest in the set.",
             **t(stems=13, spread_m=0.09, splay_deg=50, arc=0.80, width_m=0.055,
                 taper=0.5, wander=0.40, length_var=0.45, base_m=0.09,
                 head="bloom", head_m=0.10, head_share=0.22,
                 mat_stem="leaf_broadleaf", mat_head="skin_yellow",
                 bio_grassland=0.85, bio_temperate_forest=0.5,
                 place_abundance=0.45, place_spacing_m=0.9, place_cluster=0.85)),
    ),
    # --- temperate forest floor: the spring flora ---------------------------
    "common-bluebell": (
        "0.35 m — arched stalks of one-sided nodding violet bells",
        base(name="common-bluebell",
             height_m=0.35,
             notes="THE CARPET IS THE ASSET. A single bluebell is nothing and a "
                   "hectare of them is the most memorable thing in a temperate "
                   "wood, so this spec is authored to tile: very tight "
                   "spacing, near-maximum clustering, and a strongly arched "
                   "stem so the heads all lean the same way and a patch reads "
                   "as one violet sheet rather than as scattered plants.",
             **t(stems=8, spread_m=0.06, splay_deg=16, arc=0.62, width_m=0.05,
                 taper=0.7, wander=0.25, length_var=0.30, base_m=0.06,
                 head="spike", head_m=0.09, head_frac=0.34, head_share=0.55,
                 mat_stem="grass", mat_head="plume_lilac",
                 bio_temperate_forest=1.0, bio_taiga=0.2,
                 place_abundance=1.0, place_spacing_m=0.5, place_cluster=0.95,
                 place_slope_max_pct=45, place_elev_max_m=800)),
    ),
    "wood-anemone": (
        "0.24 m — one white star per stem over a whorl of leaves",
        base(name="wood-anemone",
             height_m=0.24,
             notes="AUTHORED AT 0.24 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.15 m. At 5 cm the real plant is three voxels and the "
                   "flower is under one; five voxels is where a stem, a leaf "
                   "whorl and a head can all exist. Recorded here so nobody "
                   "shrinks it back.\n\n"
                   "One head per flowering stem, a low whorl of plain stems "
                   "beneath, and it carpets -- the same placement treatment as "
                   "the bluebell, which shares the same woods a month earlier.",
             **t(stems=11, spread_m=0.08, splay_deg=38, arc=0.55, width_m=0.05,
                 taper=0.7, wander=0.40, length_var=0.30, base_m=0.08,
                 head="bloom", head_m=0.10, head_share=0.4,
                 mat_stem="leaf_broadleaf", mat_head="plume_white",
                 bio_temperate_forest=1.0, bio_taiga=0.25,
                 place_abundance=0.9, place_spacing_m=0.5, place_cluster=0.95,
                 place_elev_max_m=900)),
    ),
    "ramsons": (
        "0.40 m — broad flat leaves under white star-burst heads",
        base(name="ramsons",
             height_m=0.40,
             notes="Wild garlic, and the widest-bladed plant in the flower set: "
                   "the leaves are broad flat lances in a dense sheet, which "
                   "here is a high stem count on wide stems laid over, with a "
                   "few triangular stalks standing up through them carrying a "
                   "white burst. Another carpet species -- it excludes "
                   "everything else where it grows, so the spacing is tight and "
                   "the clustering near maximum.",
             **t(stems=12, spread_m=0.10, splay_deg=32, arc=0.62, width_m=0.09,
                 taper=0.45, wander=0.35, length_var=0.30, base_m=0.10,
                 head="bloom", head_m=0.13, head_share=0.3,
                 mat_stem="leaf_broadleaf", mat_head="plume_white",
                 bio_temperate_forest=1.0,
                 place_abundance=0.9, place_spacing_m=0.5, place_cluster=0.95,
                 place_water_max_m=40, place_elev_max_m=700)),
    ),
    "foxglove": (
        "1.50 m — one tall one-sided spike of drooping thimbles",
        base(name="foxglove",
             height_m=1.50,
             notes="The tallest thing in the flower set and a deliberately "
                   "SOLITARY one: two or three stems, a spike running half "
                   "their length, and wide spacing so it stands alone at a "
                   "clearing edge or a track side rather than in a drift. It is "
                   "the one flower here that reads at fifty metres.",
             **t(stems=3, spread_m=0.05, splay_deg=8, arc=0.12, width_m=0.07,
                 taper=0.45, wander=0.18, length_var=0.30, base_m=0.05,
                 head="spike", head_m=0.16, head_frac=0.45, head_share=0.7,
                 mat_stem="leaf_broadleaf", mat_head="leaf_blossom",
                 bio_temperate_forest=1.0, bio_taiga=0.3, bio_grassland=0.2,
                 place_abundance=0.2, place_spacing_m=2.5, place_cluster=0.5)),
    ),
    "primrose": (
        "0.24 m — flat leaf rosette, pale yellow flowers on short stalks",
        base(name="primrose",
             height_m=0.24,
             notes="AUTHORED AT 0.24 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.15 m, for the lattice: at 5 cm a 15 cm rosette is three "
                   "voxels and the flowers have nowhere to be. Written down so "
                   "it is not corrected back.\n\n"
                   "A ROSETTE, and the flowers come straight out of the middle "
                   "of it on their own short stalks rather than off a common "
                   "stem -- so the crinkled leaves are laid right over and the "
                   "heads are barely clear of them.",
             **t(stems=14, spread_m=0.10, splay_deg=52, arc=0.78, width_m=0.07,
                 taper=0.5, wander=0.35, length_var=0.35, base_m=0.10,
                 head="bloom", head_m=0.09, head_share=0.4,
                 mat_stem="leaf_broadleaf", mat_head="skin_yellow",
                 bio_temperate_forest=0.9, bio_grassland=0.4,
                 place_abundance=0.55, place_spacing_m=0.7, place_cluster=0.9)),
    ),
    "red-campion": (
        "0.70 m — branched hairy stems, flat notched pink flowers",
        base(name="red-campion",
             height_m=0.70,
             notes="The hedge-bank and wood-edge pink. Taller and looser than "
                   "the clover, with the blooms held out on branched stems "
                   "rather than sitting in the leaves -- `arc` mid, "
                   "`head_share` high, and a wide length spread so no two "
                   "flowers land at the same height.",
             **t(stems=10, spread_m=0.08, splay_deg=28, arc=0.38, width_m=0.05,
                 taper=0.65, wander=0.38, length_var=0.45, base_m=0.08,
                 head="bloom", head_m=0.11, head_share=0.55,
                 mat_stem="leaf_broadleaf", mat_head="leaf_blossom",
                 bio_temperate_forest=0.9, bio_grassland=0.45,
                 place_abundance=0.55, place_spacing_m=0.8, place_cluster=0.75)),
    ),
    "large-trillium": (
        "0.35 m — three leaves, three petals, one flower, one whorl",
        base(name="large-trillium",
             height_m=0.35,
             notes="THE CLEANEST THREE-FOLD SILHOUETTE AVAILABLE, and the only "
                   "species here authored with a stem count that low on "
                   "purpose: three broad plain stems and one flowering one. At "
                   "seven voxels tall that count IS the shape -- add stems and "
                   "it becomes a generic clump.",
             **t(stems=4, spread_m=0.06, splay_deg=42, arc=0.50, width_m=0.09,
                 taper=0.45, wander=0.20, length_var=0.18, base_m=0.06,
                 head="bloom", head_m=0.12, head_share=0.3,
                 mat_stem="leaf_broadleaf", mat_head="plume_white",
                 bio_temperate_forest=0.85,
                 place_abundance=0.35, place_spacing_m=1.0, place_cluster=0.8)),
    ),
    # --- beach: it wraps every coastline in the world ------------------------
    "sea-holly": (
        "0.50 m — rigid, spiny, leaves and flower the same blue-grey",
        base(name="sea-holly",
             height_m=0.50,
             notes="THE WHOLE PLANT IS ONE COLOUR, which is the species: leaves "
                   "and flower are the same metallic blue-grey, so the head "
                   "material and the stem material are deliberately close "
                   "rather than contrasting. That is the opposite of every "
                   "other spec on this page and it is why it looks like nothing "
                   "else on a dune.\n\n"
                   "Rigid: low arc, low wander, stiff short stems.",
             **t(stems=9, spread_m=0.08, splay_deg=30, arc=0.16, width_m=0.06,
                 taper=0.6, wander=0.18, length_var=0.30, base_m=0.08,
                 head="bloom", head_m=0.10, head_share=0.5,
                 mat_stem="leaf_needle", mat_head="skin_blue",
                 bio_beach=1.0, bio_grassland=0.2,
                 place_abundance=0.4, place_spacing_m=1.2, place_cluster=0.6,
                 place_elev_max_m=30)),
    ),
    "yellow-horned-poppy": (
        "0.60 m — grey-blue lobed leaves, big four-petal yellow cups",
        base(name="yellow-horned-poppy",
             height_m=0.60,
             notes="The shingle-bank poppy. Bigger head than the field poppy on "
                   "a shorter, looser, greyer plant, with more leaf showing -- "
                   "the seed pod that identifies it in life is a 30 cm thread "
                   "and cannot exist at 5 cm, so the read here is the flower "
                   "size against the foliage colour and nothing else.",
             **t(stems=10, spread_m=0.09, splay_deg=34, arc=0.45, width_m=0.06,
                 taper=0.6, wander=0.35, length_var=0.40, base_m=0.09,
                 head="bloom", head_m=0.15, head_share=0.4,
                 mat_stem="leaf_needle", mat_head="skin_yellow",
                 bio_beach=1.0,
                 place_abundance=0.35, place_spacing_m=1.3, place_cluster=0.6,
                 place_elev_max_m=20)),
    ),
    "sea-rocket": (
        "0.30 m — fleshy sprawling mat on bare sand, pale lilac flowers",
        base(name="sea-rocket",
             height_m=0.30,
             notes="The strandline plant, and the one that grows on bare sand "
                   "above the tide with nothing around it -- so `cluster` is "
                   "low and `spacing` wide where every other beach flower here "
                   "gathers. Fleshy: fat stems, laid over, few of them.",
             **t(stems=11, spread_m=0.11, splay_deg=48, arc=0.68, width_m=0.07,
                 taper=0.6, wander=0.45, length_var=0.35, base_m=0.11,
                 head="bloom", head_m=0.08, head_share=0.45,
                 mat_stem="leaf_broadleaf", mat_head="plume_lilac",
                 bio_beach=1.0,
                 place_abundance=0.3, place_spacing_m=1.8, place_cluster=0.35,
                 place_elev_max_m=8)),
    ),
    "sea-lavender": (
        "0.40 m — flat sprays of tiny papery lilac flowers",
        base(name="sea-lavender",
             height_m=0.40,
             notes="Salt marsh. A wide flat spray on a bare wiry stem over a "
                   "basal rosette -- so most stems are plain and laid over, and "
                   "the few that flower carry a head much wider than they are "
                   "thick. It grows in sheets, so the spacing is tight.",
             **t(stems=12, spread_m=0.08, splay_deg=36, arc=0.40, width_m=0.05,
                 taper=0.7, wander=0.30, length_var=0.35, base_m=0.08,
                 head="bloom", head_m=0.18, head_share=0.35,
                 mat_stem="leaf_dry", mat_head="plume_lilac",
                 bio_beach=1.0, bio_grassland=0.15,
                 place_abundance=0.6, place_spacing_m=0.6, place_cluster=0.9,
                 place_water_max_m=30, place_elev_max_m=6)),
    ),
    "sea-kale": (
        "0.70 m — big cabbage mound of blue-grey leaves, white flower cloud",
        base(name="sea-kale",
             height_m=0.70,
             notes="The largest flowering ground plant in the library and "
                   "deliberately a MOUND rather than a tuft: very wide root "
                   "spread, broad thick stems, heavy arc, and a white head "
                   "cloud sitting on top of it. On a shingle bank it is the "
                   "only thing with volume, so it is authored to have some.",
             **t(stems=13, spread_m=0.18, splay_deg=44, arc=0.62, width_m=0.11,
                 taper=0.45, wander=0.35, length_var=0.35, base_m=0.18,
                 head="bloom", head_m=0.20, head_share=0.35,
                 mat_stem="leaf_needle", mat_head="plume_white",
                 bio_beach=1.0,
                 place_abundance=0.25, place_spacing_m=2.2, place_cluster=0.5,
                 place_elev_max_m=12)),
    ),
    "sea-bindweed": (
        "0.22 m — prostrate runners over sand with pink trumpets",
        base(name="sea-bindweed",
             height_m=0.22,
             notes="AUTHORED AT 0.22 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.15 m, which is the library's floor for a thing that has "
                   "to be drawn at all: at 5 cm 0.15 m is three voxels. Said "
                   "here so it is not shrunk.\n\n"
                   "The flattest plant in the set -- runners, not stems. The "
                   "widest root spread and the highest splay on this page, so "
                   "the whole thing lies on the sand and only the trumpets "
                   "stand.",
             **t(stems=18, spread_m=0.16, splay_deg=66, arc=0.90, width_m=0.05,
                 taper=0.7, wander=0.60, length_var=0.30, base_m=0.16,
                 head="bloom", head_m=0.10, head_share=0.35,
                 mat_stem="leaf_broadleaf", mat_head="leaf_blossom",
                 bio_beach=1.0,
                 place_abundance=0.4, place_spacing_m=1.2, place_cluster=0.7,
                 place_elev_max_m=10)),
    ),
    # --- desert --------------------------------------------------------------
    "desert-marigold": (
        "0.30 m — woolly grey clump under a floating layer of yellow discs",
        base(name="desert-marigold",
             height_m=0.30,
             notes="Many thin naked stalks all reaching the SAME height, which "
                   "is why `length_var` is the lowest in the set: the flowers "
                   "read as a single floating yellow layer over a woolly grey "
                   "clump, and any spread in stem length breaks that.",
             **t(stems=14, spread_m=0.08, splay_deg=24, arc=0.20, width_m=0.05,
                 taper=0.75, wander=0.25, length_var=0.12, base_m=0.08,
                 head="bloom", head_m=0.11, head_share=0.6,
                 mat_stem="leaf_needle", mat_head="skin_yellow",
                 bio_desert=1.0, bio_savanna=0.35, bio_grassland=0.2,
                 place_abundance=0.3, place_spacing_m=1.6, place_cluster=0.55)),
    ),
    "globe-mallow": (
        "0.60 m — grey woolly stems with orange cups spaced up them",
        base(name="globe-mallow",
             height_m=0.60,
             notes="The flowers are spaced ALONG the upper stem rather than "
                   "clustered at the top, which the tuft generator says with a "
                   "long `head_frac` spike rather than a bloom -- a bloom would "
                   "put one knob on each tip and lose the whole arrangement.",
             **t(stems=8, spread_m=0.07, splay_deg=26, arc=0.30, width_m=0.055,
                 taper=0.6, wander=0.30, length_var=0.40, base_m=0.07,
                 head="spike", head_m=0.11, head_frac=0.42, head_share=0.6,
                 mat_stem="leaf_needle", mat_head="skin_orange",
                 bio_desert=1.0, bio_savanna=0.3,
                 place_abundance=0.3, place_spacing_m=1.8, place_cluster=0.5)),
    ),
    "desert-lily": (
        "0.50 m — wavy strap leaves, one stalk of big white trumpets",
        base(name="desert-lily",
             height_m=0.50,
             notes="Strap leaves and ONE thick flowering stalk, so nearly every "
                   "stem is plain and wide and one is narrow and vertical "
                   "carrying a run of large white trumpets up one side. The "
                   "biggest head-to-plant ratio in the desert set, which is "
                   "what an ephemeral desert flower is for.",
             **t(stems=9, spread_m=0.09, splay_deg=40, arc=0.55, width_m=0.08,
                 taper=0.45, wander=0.35, length_var=0.35, base_m=0.09,
                 head="spike", head_m=0.15, head_frac=0.35, head_share=0.25,
                 mat_stem="leaf_needle", mat_head="plume_white",
                 bio_desert=1.0,
                 place_abundance=0.2, place_spacing_m=2.5, place_cluster=0.6)),
    ),
    "sand-verbena": (
        "0.22 m — sticky flat mat with tight round magenta heads",
        base(name="sand-verbena",
             height_m=0.22,
             notes="AUTHORED AT 0.22 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.10 m. At 5 cm the real plant is two voxels: a mat with no "
                   "room for a head on it. Written down rather than left to be "
                   "rediscovered.\n\n"
                   "The heads sit DIRECTLY on the mat with almost no vertical "
                   "extent, so the flowers are drawn on stems laid nearly flat "
                   "and the plant reads as a coloured patch of ground.",
             **t(stems=18, spread_m=0.15, splay_deg=62, arc=0.88, width_m=0.05,
                 taper=0.7, wander=0.55, length_var=0.25, base_m=0.15,
                 head="bloom", head_m=0.09, head_share=0.55,
                 mat_stem="leaf_needle", mat_head="leaf_blossom",
                 bio_desert=1.0, bio_beach=0.4,
                 place_abundance=0.35, place_spacing_m=1.4, place_cluster=0.8)),
    ),
    # --- savanna: 20.76% of land, and it had one flower spec -----------------
    "fire-lily": (
        "0.40 m — one bare stem out of bare ground, flame-red trumpets",
        base(name="fire-lily",
             height_m=0.40,
             notes="NO LEAVES AT ALL, which is the species: it comes up after a "
                   "burn on a single bare stem, so `head_share` is 1.0 and "
                   "there are almost no stems. That makes it the only spec on "
                   "this page with no foliage, and the reason the tuft "
                   "generator's leaves-for-free trick is a slider rather than a "
                   "constant.",
             **t(stems=3, spread_m=0.04, splay_deg=10, arc=0.14, width_m=0.06,
                 taper=0.6, wander=0.18, length_var=0.25, base_m=0.04,
                 head="bloom", head_m=0.13, head_share=1.0,
                 mat_stem="leaf_dry", mat_head="plume_crimson",
                 bio_savanna=1.0, bio_grassland=0.25,
                 place_abundance=0.25, place_spacing_m=1.8, place_cluster=0.7)),
    ),
    "wild-gerbera": (
        "0.30 m — flat ground rosette, two or three big orange daisies",
        base(name="wild-gerbera",
             height_m=0.30,
             notes="A rosette pressed to the ground with two or three long bare "
                   "stalks out of it, each carrying one large flat head. Same "
                   "structure as the cowslip and a completely different read, "
                   "because the head is half as wide again and the stalks are "
                   "bare -- proportion, not parts.",
             **t(stems=10, spread_m=0.09, splay_deg=54, arc=0.72, width_m=0.07,
                 taper=0.5, wander=0.35, length_var=0.40, base_m=0.09,
                 head="bloom", head_m=0.15, head_share=0.3,
                 mat_stem="savanna_grass", mat_head="skin_orange",
                 bio_savanna=1.0, bio_grassland=0.3,
                 place_abundance=0.35, place_spacing_m=1.4, place_cluster=0.6)),
    ),
    # --- taiga ---------------------------------------------------------------
    "fireweed": (
        "1.20 m — single tall stem, long magenta spike open from the bottom",
        base(name="fireweed",
             height_m=1.20,
             notes="The boreal clearing and burn plant, and one of only two "
                   "things on this page over a metre. A single unbranched stem "
                   "with a long spike -- so a stand of it is a field of "
                   "vertical magenta bars, which is exactly how it reads after "
                   "a fire.",
             **t(stems=4, spread_m=0.05, splay_deg=10, arc=0.14, width_m=0.06,
                 taper=0.5, wander=0.20, length_var=0.35, base_m=0.05,
                 head="spike", head_m=0.13, head_frac=0.42, head_share=0.8,
                 mat_stem="leaf_broadleaf", mat_head="leaf_blossom",
                 bio_taiga=1.0, bio_temperate_forest=0.4,
                 bio_tundra_alpine=0.3,
                 place_abundance=0.5, place_spacing_m=1.0, place_cluster=0.9)),
    ),
    "marsh-marigold": (
        "0.30 m — glossy kidney leaves at the water's edge, gold cups",
        base(name="marsh-marigold",
             height_m=0.30,
             notes="A waterside plant and authored as one: it only places "
                   "within a few metres of water and on nearly flat ground. "
                   "Broad glossy leaves laid over, large waxy heads just clear "
                   "of them.",
             **t(stems=12, spread_m=0.10, splay_deg=44, arc=0.66, width_m=0.09,
                 taper=0.5, wander=0.35, length_var=0.30, base_m=0.10,
                 head="bloom", head_m=0.12, head_share=0.4,
                 mat_stem="leaf_broadleaf", mat_head="skin_yellow",
                 bio_taiga=0.9, bio_temperate_forest=0.6,
                 bio_tundra_alpine=0.3,
                 place_abundance=0.5, place_spacing_m=0.7, place_cluster=0.9,
                 place_water_max_m=6, place_slope_max_pct=15)),
    ),
    "cloudberry": (
        "0.24 m — sparse crinkled leaves, one amber fruit per stem",
        base(name="cloudberry",
             height_m=0.24,
             notes="AUTHORED AT 0.24 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.20 m, to clear the five-voxel floor with a head on top. "
                   "Bog surfaces only -- flat ground, near water, heavily "
                   "clustered.\n\n"
                   "The head here is the FRUIT rather than the flower, which is "
                   "how anyone recognises it: a single amber-orange berry per "
                   "stem over very few broad leaves.",
             **t(stems=7, spread_m=0.08, splay_deg=34, arc=0.48, width_m=0.08,
                 taper=0.5, wander=0.30, length_var=0.25, base_m=0.08,
                 head="bloom", head_m=0.08, head_share=0.45,
                 mat_stem="leaf_broadleaf", mat_head="skin_orange",
                 bio_taiga=1.0, bio_tundra_alpine=0.5,
                 place_abundance=0.45, place_spacing_m=0.6, place_cluster=0.95,
                 place_water_max_m=20, place_slope_max_pct=12)),
    ),
    # --- tundra / alpine -----------------------------------------------------
    "purple-saxifrage": (
        "0.22 m — trailing mat under big magenta five-petal flowers",
        base(name="purple-saxifrage",
             height_m=0.22,
             notes="AUTHORED AT 0.22 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.05 m, which is the largest departure on this page and the "
                   "one most likely to be 'fixed'. At 5 cm the real plant is "
                   "ONE VOXEL. It is here because it is the earliest colour of "
                   "the alpine year and a biome that is otherwise grey, white "
                   "and dun cannot spare it.\n\n"
                   "The identifying ratio survives the enlargement, and that "
                   "ratio is the point: an alpine flower is disproportionately "
                   "large-flowered for its size, so `head_m` is nearly half the "
                   "plant's height.",
             **t(stems=20, spread_m=0.14, splay_deg=64, arc=0.88, width_m=0.05,
                 taper=0.75, wander=0.55, length_var=0.25, base_m=0.14,
                 head="bloom", head_m=0.10, head_share=0.6,
                 mat_stem="leaf_needle", mat_head="plume_lilac",
                 bio_tundra_alpine=1.0, bio_taiga=0.3,
                 place_abundance=0.5, place_spacing_m=0.7, place_cluster=0.9,
                 place_elev_min_m=600, place_slope_max_pct=65)),
    ),
    "arctic-poppy": (
        "0.25 m — one curved hairy stalk, one nodding pale-yellow bowl",
        base(name="arctic-poppy",
             height_m=0.25,
             notes="A STEM AND A CUP AND NOTHING ELSE, which is the whole "
                   "silhouette: three or four stems, no side branches, one head "
                   "each. The slight curve is `wander` rather than `arc`, "
                   "because the stalk leans without laying its flower over.",
             **t(stems=4, spread_m=0.05, splay_deg=18, arc=0.26, width_m=0.05,
                 taper=0.7, wander=0.45, length_var=0.30, base_m=0.05,
                 head="bloom", head_m=0.11, head_share=0.8,
                 mat_stem="leaf_needle", mat_head="skin_yellow",
                 bio_tundra_alpine=1.0,
                 place_abundance=0.35, place_spacing_m=1.1, place_cluster=0.7,
                 place_elev_min_m=500)),
    ),
    "mountain-avens": (
        "0.22 m — woody mat of crinkled leaves, white eight-petal flowers",
        base(name="mountain-avens",
             height_m=0.22,
             notes="AUTHORED AT 0.22 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.12 m, for the five-voxel floor. Written down so it stays.\n\n"
                   "A woody mat rather than a cushion: the stems are stiffer "
                   "and the arc lower than `purple-saxifrage` beside it, so the "
                   "two do not read as one plant in two colours.",
             **t(stems=16, spread_m=0.12, splay_deg=54, arc=0.70, width_m=0.055,
                 taper=0.65, wander=0.40, length_var=0.30, base_m=0.12,
                 head="bloom", head_m=0.10, head_share=0.5,
                 mat_stem="leaf_needle", mat_head="plume_white",
                 bio_tundra_alpine=1.0, bio_taiga=0.25,
                 place_abundance=0.5, place_spacing_m=0.8, place_cluster=0.85,
                 place_elev_min_m=500, place_slope_max_pct=60)),
    ),
    "edelweiss": (
        "0.24 m — flat star of white woolly bracts on a grey felted stem",
        base(name="edelweiss",
             height_m=0.24,
             notes="A FLAT STAR, so the head is wide and the stem short: "
                   "`head_m` is 0.13 on a 0.24 m plant, the highest head-to-"
                   "height ratio in the library, which is the exaggerated "
                   "alpine proportion the tundra file asks for.\n\n"
                   "The whiteness is felt rather than petal, and `plume_white` "
                   "is the flat matte white in the palette -- `snow` has no "
                   "per-voxel jitter at all and reads as a printed decal.",
             **t(stems=8, spread_m=0.07, splay_deg=30, arc=0.30, width_m=0.055,
                 taper=0.6, wander=0.28, length_var=0.25, base_m=0.07,
                 head="bloom", head_m=0.13, head_share=0.55,
                 mat_stem="leaf_needle", mat_head="plume_white",
                 bio_tundra_alpine=1.0,
                 place_abundance=0.25, place_spacing_m=1.3, place_cluster=0.65,
                 place_elev_min_m=800, place_slope_max_pct=65)),
    ),
    "trumpet-gentian": (
        "0.22 m — one deep-blue trumpet nearly as long as the plant",
        base(name="trumpet-gentian",
             height_m=0.22,
             notes="AUTHORED AT 0.22 m AGAINST THE BIOME LIST'S APPROXIMATE "
                   "0.08 m. At 5 cm the real plant is under two voxels. Kept "
                   "because it is the most saturated blue in the alpine band "
                   "and the palette now has somewhere to put it.\n\n"
                   "The flower is nearly as long as the plant is tall, so "
                   "`head_m` is over half the height and `head_share` is high: "
                   "a flat leaf rosette with trumpets standing straight out of "
                   "it.",
             **t(stems=9, spread_m=0.08, splay_deg=48, arc=0.62, width_m=0.06,
                 taper=0.55, wander=0.30, length_var=0.20, base_m=0.08,
                 head="bloom", head_m=0.12, head_share=0.55,
                 mat_stem="leaf_broadleaf", mat_head="skin_blue",
                 bio_tundra_alpine=1.0,
                 place_abundance=0.3, place_spacing_m=1.0, place_cluster=0.8,
                 place_elev_min_m=600)),
    ),
    # --- rainforest understorey ---------------------------------------------
    "heliconia": (
        "1.60 m — paddle leaves and a hanging zigzag of scarlet bracts",
        base(name="heliconia",
             height_m=1.60,
             notes="THE BRACTS ARE THE MARK, not the flower, and they are "
                   "stacked down a hanging stem -- so this is a spike with a "
                   "long `head_frac`, carried by a small share of very broad "
                   "paddle stems. It is the tallest flower in the library after "
                   "the foxglove and the one that fixes what the rainforest "
                   "file calls its head-height gap: a canopy and a floor and "
                   "almost nothing between them.",
             **t(stems=7, spread_m=0.12, splay_deg=28, arc=0.42, width_m=0.11,
                 taper=0.5, wander=0.25, length_var=0.35, base_m=0.12,
                 head="spike", head_m=0.20, head_frac=0.40, head_share=0.3,
                 mat_stem="leaf_jungle", mat_head="plume_crimson",
                 bio_rainforest=1.0,
                 place_abundance=0.4, place_spacing_m=1.6, place_cluster=0.75)),
    ),
    "wild-ginger": (
        "1.20 m — cane stems with a tight red bract cone near the ground",
        base(name="wild-ginger",
             height_m=1.20,
             notes="The cone sits BENEATH the leaves rather than above them, "
                   "which nothing in the tuft generator can say directly -- so "
                   "it is drawn as a short heavy head on a small share of very "
                   "short stems under a canopy of tall broad ones. The result "
                   "reads as a red knot at the base of a cane clump, which is "
                   "the recognisable thing about it.",
             **t(stems=10, spread_m=0.12, splay_deg=22, arc=0.36, width_m=0.10,
                 taper=0.55, wander=0.25, length_var=0.55, base_m=0.12,
                 head="bloom", head_m=0.16, head_share=0.25,
                 mat_stem="leaf_jungle", mat_head="plume_crimson",
                 bio_rainforest=1.0,
                 place_abundance=0.4, place_spacing_m=1.5, place_cluster=0.8)),
    ),
    "impatiens": (
        "0.35 m — untidy soft mound of small flat pink and white blooms",
        base(name="impatiens",
             height_m=0.35,
             notes="MANY SMALL HEADS rather than a few big ones, which is the "
                   "opposite of every alpine on this page and is what a wet "
                   "shaded understorey plant looks like. The highest stem count "
                   "and the highest head share together, on the smallest head "
                   "width the generator will draw as a bloom.",
             **t(stems=22, spread_m=0.10, splay_deg=40, arc=0.58, width_m=0.05,
                 taper=0.7, wander=0.45, length_var=0.40, base_m=0.10,
                 head="bloom", head_m=0.07, head_share=0.7,
                 mat_stem="leaf_jungle", mat_head="leaf_blossom",
                 bio_rainforest=1.0, bio_temperate_forest=0.2,
                 place_abundance=0.6, place_spacing_m=0.8, place_cluster=0.85,
                 place_water_max_m=40)),
    ),
    "terrestrial-bromeliad": (
        "0.50 m — stiff strap rosette arching back, one red spike from the middle",
        base(name="terrestrial-bromeliad",
             height_m=0.50,
             notes="TERRESTRIAL ON PURPOSE. Most rainforest bromeliads and all "
                   "its orchids live on branches, and nothing in `forge/kinds.py` "
                   "attaches an asset to a tree -- so this is the ground-living "
                   "kind, and the epiphyte look stays an open placement "
                   "question rather than being faked here.\n\n"
                   "A rosette whose leaf tips arch back DOWN is a high `arc` on "
                   "stiff wide stems with a low splay: they leave the crown "
                   "steeply and fall away, which is the opposite of a grass "
                   "tuft's low-and-out. One narrow stem through the middle "
                   "carries the spike.",
             **t(stems=11, spread_m=0.09, splay_deg=16, arc=0.82, width_m=0.10,
                 taper=0.4, wander=0.20, length_var=0.25, base_m=0.09,
                 head="spike", head_m=0.12, head_frac=0.30, head_share=0.15,
                 mat_stem="leaf_jungle", mat_head="plume_crimson",
                 bio_rainforest=1.0,
                 place_abundance=0.35, place_spacing_m=1.4, place_cluster=0.7)),
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

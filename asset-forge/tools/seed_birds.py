"""Author the first twenty bird species.

One-off, and it refuses to overwrite a spec that already exists -- see
`tools/seedspec.py` for why (a seed script silently reverted a finished hero
back to its draft values, and the only reason it was recoverable is that a
backup happened to be seconds old).

WHAT THIS SET IS FOR. Twenty species chosen to span the world's biomes and the
readable range of bird SHAPES, not to cover ornithology. Every one of the ten
biomes the engine classifies carries at least one, including the two that host
nothing else: open ocean, which had only fish in it, and bare rock, which had
only boulders. The point of the set is that no two of them can be mistaken for
each other AT TWENTY-FIVE VOXELS, which is a much stronger constraint than
looking different in a drawing.

The shape spread, deliberately:

  proportion   heron (neck 0.26 of its length) against starling (0.03)
  tail         macaw (0.55, graduated) against kingfisher (0.11)
  posture      woodpecker (68 degrees, up a trunk) against mallard (4, level)
  bill         heron and kingfisher daggers against a hawfinch-grade cone,
               a hooked eagle, a chisel, a spatula and a decurved hoopoe
  head         owl (head_size 1.42, no neck) against heron (0.85, all neck)
  wing         all four planforms, folded on fifteen and spread on five
  legs         heron 0.30 of its length against swift-grade 0.03

Every one is authored at **1 cm**. See `docs/bird-shape-research.md`: at the
5 cm asset lattice a 24 cm robin is five voxels long and there is no bird
there, and at 2 cm it is twelve, which cannot carry a bill and an eye. 1 cm
nests 10:1 in the terrain lattice and 5:1 in the asset lattice, both whole
numbers, and a whole bird is 334 to 28,355 voxels -- a flock of forty
song-thrushes is 1.25% of one `temperate-oak`, which is 1,065,343.

THE SIZE FLOOR. Nothing here is under 20 cm, and four species are authored
ABOVE their real length to reach it. Each of those says so in its own `notes`,
because a note is what stops the next person "correcting" it back.

TEN OF THE TWENTY BILLS WERE INVISIBLE, and it took a fourth contrast check to
find them. `tools/birdprobe.py --read` originally checked the three MARKINGS
against what they sit on and did not check the bill against the head. When that
check was added, ten species failed it: a robin's horn-coloured bill on its
olive head measured a contrast ratio of 1.04, a great tit's black bill on its
black cap measured 1.00, a raven's and a swallow's measured 1.40. The bill was
there in every case and could not be seen.

That matters more than a marking does. *Pixel Logic* records that Super Mario
World's Swoopers are bats which read as birds purely because their nose was
coloured orange; Minecraft gives its chicken's beak its own box; shipped 16x16
sprite packs advertise "blue body, yellow beak". The bill is the cheapest
identifying feature a bird has. So twelve of the twenty now carry a bright bill
and three a grey one, and three of the songbirds had their bill share raised too,
because two voxels of bill is the floor at which one exists at all.

COLOUR IS PUSHED PAST LIFE. The brief was "colourful and stylised", and the
research says why that is not merely a preference: melanin accounts for 74% of
the plumage patches on a bird and only 7% of the colour gamut, while structural
colour is 7% of the patches and 45% of the gamut (Delhey 2015, 46,559 spectra
over 555 species). A palette weighted by AREA, which is what copying a field
guide gives you, is browns and greys. So the rare colours are deliberately
over-weighted here: a raven carries its gloss as a real teal, a pigeon's neck
is really lilac, a kingfisher is turquoise rather than the deep blue it
photographs as. Where a species is pushed, its notes say so.

    python tools/seed_birds.py
    python tools/seed_birds.py --force     # revert them all to these drafts

MORPHOMETRIC SOURCE. The proportions below are read off published medians, not
guessed, and mostly off AVONET (Tobias et al. 2022, Ecology Letters 25:581-597,
doi:10.1111/ele.13898, figshare 10.6084/m9.figshare.16586228) -- which, unlike
every fish dataset the fish work looked at, is cleanly **CC BY 4.0** and usable
in a commercial product. Nothing from it is embedded: what is here is twenty
species' worth of ratios typed out by hand. See the research doc for why the
dataset was still not vendored.
"""
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
import seedspec
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"


def base(**over):
    """A spec with every bird default, then the species' own values."""
    changes = {
        "kind": "bird",
        "resolution_cm": "1",
        # A bird is small and cheap, so it can afford to vary widely; a flock of
        # identical animals is the single most obvious tell that something is
        # generated, and a flock is far more visible than a shoal. These four
        # are the ones `bird._params` actually reads.
        "variation.amount": 1.0,
        "variation.height": 0.14,
        "variation.shape": 0.16,
        "variation.proportion": 0.18,
        "flock.entity_class": "detail",
    }
    changes.update(over)
    return changes


def b(**kw):
    """`bird.*` and `materials.bird_*` from keyword arguments.

    `mat_back=` becomes `materials.bird_back`; everything else gets the `bird.`
    prefix. Python keywords cannot contain a dot, which is the whole reason for
    the translation, and twenty species times forty rows is enough repetition to
    make it worth having.
    """
    out = {}
    for k, v in kw.items():
        if k.startswith("mat_"):
            out["materials.bird_" + k[len("mat_"):]] = v
        elif k.startswith("flock_"):
            out["flock." + k[len("flock_"):]] = v
        elif k.startswith("bio_"):
            out["biomes." + k[len("bio_"):]] = v
        elif k.startswith("place_"):
            out["placement." + k[len("place_"):]] = v
        else:
            out["bird." + k] = v
    return out


# name -> (blurb, changes)
SPECIES = {
    # --- corvids ------------------------------------------------------------
    "common-raven": (
        "the big black one: wedge tail, heavy bill, teal gloss",
        base(
            name="common-raven",
            notes="A raven is identified at any distance by three things and "
                  "none of them is colour: it is big, its tail is a WEDGE "
                  "rather than a fan, and its bill is heavy enough to see in "
                  "silhouette.\n\n"
                  "COLOUR PUSHED. A real raven is black with a faint blue-green "
                  "sheen you only see in sunlight. Drawn that way it is a black "
                  "lozenge and it loses to every other species on a sheet. Here "
                  "the gloss is a real dark teal over a black underside, with a "
                  "violet wing panel. It is still unmistakably the black bird, "
                  "and it now has a top and a bottom.",
            **b(length_m=0.64, bill_frac=0.115, head_frac=0.135, neck_frac=0.05,
                body_frac=0.36, tail_frac=0.34, posture_deg=22,
                body_depth=0.70, body_width=0.68, chest_at=0.34, breast=0.70,
                rump=0.44, fullness=3.0, head_size=1.05, neck_up_deg=24,
                neck_thick=0.55, section=2.1, belly=0.52,
                bill_depth=0.42, bill_curve=0.10, bill_hook=0.22, bill_gape=0.22,
                tail_shape="wedge", tail_width=0.52, tail_droop=0.55,
                tail_thick=2,
                pose="perched", wing_shape="elliptical", wing_span=2.05,
                wing_aspect=5.9, wing_fold=0.60, wing_thick=2,
                leg_len=0.11, eye=1.0, upperparts=0.60,
                head_mark="none", wing_mark="panel", body_mark="none",
                mark_width=0.26,
                mat_back="plume_iridescent", mat_belly="skin_dark",
                mat_head="plume_iridescent", mat_wing="skin_dark",
                mat_mark="plume_lilac", mat_head_mark="plume_slate",
                mat_bill="plume_grey", mat_eye="plume_white",
                bio_taiga=0.9, bio_temperate_forest=0.7, bio_tundra_alpine=0.6,
                bio_bare_rock=0.5, bio_grassland=0.3, bio_desert=0.2,
                place_abundance=0.30, place_spacing_m=120.0,
                place_slope_max_pct=70.0,
                flock_despawn_m=160.0, flock_size_min=1, flock_size_max=4,
                flock_spread_m=40.0, flock_perch="cliff",
                flock_height_min_m=8.0, flock_height_max_m=250.0,
                flock_flight_share=0.45, flock_per_hectare=0.3)),
    ),
    "eurasian-jay": (
        "crested corvid, rufous body, electric barred wing panel",
        base(
            name="eurasian-jay",
            notes="The corvid that is not black, and the one the crest exists "
                  "for. Its identity is a rufous body with a BLOCK OF BLUE on "
                  "the wing -- the only wing panel in the set strong enough to "
                  "read from behind.\n\n"
                  "COLOUR PUSHED. The real wing patch is a small barred panel "
                  "of pale blue about two centimetres across. Here it is the "
                  "whole outer wing in turquoise with dark bars. Aspect ratio "
                  "4.5 is the lowest measured in Alerstam's 129 species and it "
                  "is why the wing is so short and round.",
            **b(length_m=0.34, bill_frac=0.085, head_frac=0.145, neck_frac=0.04,
                body_frac=0.35, tail_frac=0.38, posture_deg=32,
                body_depth=0.78, body_width=0.68, chest_at=0.33, breast=0.72,
                rump=0.46, fullness=3.2, head_size=1.0, neck_up_deg=32,
                neck_thick=0.52, section=2.1, belly=0.52, crest=0.32,
                bill_depth=0.36, bill_hook=0.12, bill_gape=0.18,
                tail_shape="rounded", tail_width=0.46, tail_droop=0.55,
                pose="perched", wing_shape="elliptical", wing_span=1.75,
                wing_aspect=4.5, wing_fold=0.42,
                leg_len=0.12, eye=1.0, upperparts=0.46,
                head_mark="mask", wing_mark="panel", body_mark="none",
                mark_width=0.22,
                mat_back="plume_rufous", mat_belly="plume_buff",
                mat_head="plume_rufous", mat_wing="plume_cyan",
                mat_mark="skin_dark", mat_head_mark="skin_dark",
                mat_bill="skin_dark", mat_eye="skin_dark",
                bio_temperate_forest=0.9, bio_taiga=0.4, bio_grassland=0.25,
                place_abundance=0.45, place_spacing_m=45.0,
                flock_despawn_m=90.0, flock_size_min=1, flock_size_max=3,
                flock_spread_m=25.0, flock_perch="canopy",
                flock_height_min_m=3.0, flock_height_max_m=30.0,
                flock_flight_share=0.30, flock_per_hectare=1.2)),
    ),
    # --- songbirds ----------------------------------------------------------
    "european-robin": (
        "the small round one with the orange front",
        base(
            name="european-robin",
            notes="The reference small songbird, and the test of how little "
                  "will do: an upright round body, a thin bill, a square tail "
                  "and ONE block of colour on the front.\n\n"
                  "AUTHORED AT 24 cm, NOT LIFE SIZE. A robin is 14 cm, which "
                  "at the 1 cm lattice is fourteen voxels -- no bill, no eye "
                  "and no tail shape. 20 cm is this library's floor and this "
                  "one is above it, because a robin sits at 42 degrees "
                  "nose-up and 20 cm of bird at that angle projects onto 16 "
                  "voxels of length. `tools/birdprobe.py --read` flagged it "
                  "SHORT at 20 cm and passes it at 24.",
            **b(length_m=0.24, bill_frac=0.085, head_frac=0.165, neck_frac=0.035,
                body_frac=0.38, tail_frac=0.32, posture_deg=42,
                body_depth=0.92, body_width=0.66, chest_at=0.32, breast=0.76,
                rump=0.48, fullness=3.6, head_size=1.05, neck_up_deg=42,
                neck_thick=0.50, section=2.2, belly=0.54,
                bill_depth=0.24, bill_gape=0.12,
                tail_shape="square", tail_width=0.40, tail_droop=0.45,
                pose="perched", wing_shape="elliptical", wing_span=1.55,
                wing_aspect=5.4, wing_fold=0.30,
                leg_len=0.13, eye=1.0, upperparts=0.52,
                head_mark="throat", wing_mark="none", body_mark="none",
                mark_width=0.22,
                mat_back="skin_olive", mat_belly="plume_white",
                mat_head="skin_olive", mat_wing="skin_olive",
                mat_mark="plume_buff", mat_head_mark="skin_orange",
                mat_bill="skin_yellow", mat_eye="skin_dark",
                bio_temperate_forest=0.9, bio_grassland=0.45, bio_taiga=0.35,
                place_abundance=0.85, place_spacing_m=12.0,
                place_slope_max_pct=60.0,
                flock_despawn_m=60.0, flock_size_min=1, flock_size_max=2,
                flock_spread_m=6.0, flock_perch="shrub",
                flock_height_min_m=0.5, flock_height_max_m=8.0,
                flock_flight_share=0.20, flock_per_hectare=8.0)),
    ),
    "great-tit": (
        "black cap, white cheek, lime back, yellow front, white wing bar",
        base(
            name="great-tit",
            notes="Four blocks of colour on twenty voxels, which is the most "
                  "any species in the set carries and the reason the marking "
                  "system is three regions rather than one. The cap and the "
                  "cheek are head marks; the wing bar is a wing mark; the "
                  "yellow underparts are the base. None of them overlaps, "
                  "which is exactly why a bird may carry three marks where a "
                  "fish may carry one.\n\n"
                  "AUTHORED AT 24 cm rather than its real 14, for the same "
                  "reason as the robin: 20 cm of bird sitting at 36 degrees "
                  "nose-up projects onto 17 voxels and the readability gate "
                  "flags it SHORT.",
            **b(length_m=0.24, bill_frac=0.080, head_frac=0.170, neck_frac=0.030,
                body_frac=0.375, tail_frac=0.33, posture_deg=36,
                body_depth=0.88, body_width=0.66, chest_at=0.32, breast=0.74,
                rump=0.46, fullness=3.5, head_size=1.10, neck_up_deg=38,
                neck_thick=0.55, section=2.2, belly=0.53,
                bill_depth=0.30, bill_gape=0.14,
                tail_shape="notched", tail_width=0.42, tail_fork=0.30,
                tail_droop=0.50,
                pose="perched", wing_shape="elliptical", wing_span=1.55,
                wing_aspect=5.2, wing_fold=0.32,
                leg_len=0.12, eye=1.0, upperparts=0.55,
                head_mark="mask", wing_mark="bar", body_mark="none",
                mark_width=0.16,
                mat_back="plume_lime", mat_belly="skin_yellow",
                mat_head="skin_dark", mat_wing="plume_slate",
                mat_mark="plume_white", mat_head_mark="plume_white",
                mat_bill="plume_grey", mat_eye="skin_dark",
                bio_temperate_forest=0.9, bio_taiga=0.35, bio_grassland=0.3,
                place_abundance=0.9, place_spacing_m=10.0,
                flock_despawn_m=60.0, flock_size_min=2, flock_size_max=10,
                flock_spread_m=12.0, flock_perch="canopy",
                flock_height_min_m=1.0, flock_height_max_m=14.0,
                flock_flight_share=0.30, flock_per_hectare=12.0)),
    ),
    "song-thrush": (
        "warm brown above, cream below, spotted all over the front",
        base(
            name="song-thrush",
            notes="The species the SPECKLED body marking exists for, and the "
                  "one that proves the quantile threshold matters: the "
                  "coverage slider is the exact share of the bird that is "
                  "spotted, so 0.32 is 32% and not whatever the noise happened "
                  "to do that seed. Also the only bird in the set whose "
                  "identity is a texture rather than a block.",
            **b(length_m=0.23, bill_frac=0.090, head_frac=0.150, neck_frac=0.035,
                body_frac=0.375, tail_frac=0.335, posture_deg=34,
                body_depth=0.84, body_width=0.68, chest_at=0.32, breast=0.74,
                rump=0.46, fullness=3.3, head_size=1.0, neck_up_deg=36,
                neck_thick=0.52, section=2.1, belly=0.53,
                bill_depth=0.26, bill_gape=0.13,
                tail_shape="square", tail_width=0.42, tail_droop=0.50,
                pose="perched", wing_shape="elliptical", wing_span=1.60,
                wing_aspect=5.6, wing_fold=0.40,
                leg_len=0.13, eye=1.0, upperparts=0.56,
                head_mark="none", wing_mark="none", body_mark="speckled",
                mark_width=0.09, mark_strength=0.32,
                mat_back="plume_rufous", mat_belly="plume_white",
                mat_head="plume_rufous", mat_wing="plume_rufous",
                mat_mark="skin_dark", mat_head_mark="skin_dark",
                mat_bill="skin_yellow", mat_eye="skin_dark",
                bio_temperate_forest=0.85, bio_grassland=0.5, bio_taiga=0.3,
                place_abundance=0.6, place_spacing_m=25.0,
                flock_despawn_m=70.0, flock_size_min=1, flock_size_max=2,
                flock_spread_m=10.0, flock_perch="canopy",
                flock_height_min_m=1.0, flock_height_max_m=16.0,
                flock_flight_share=0.22, flock_per_hectare=3.0)),
    ),
    "common-starling": (
        "glossy dark green, speckled, short square tail, sharp yellow bill",
        base(
            name="common-starling",
            notes="A starling is a SHAPE before it is a colour: a short square "
                  "tail on a triangular body with a spike of a bill, which is "
                  "why it is unmistakable in flight and why the tail share is "
                  "the lowest of any perching bird here at 0.22.\n\n"
                  "COLOUR PUSHED. The real gloss is a purple-green over black "
                  "and it only fires in direct sun. Here it is a saturated dark "
                  "teal over the whole animal with white speckles, which is the "
                  "one lighting condition drawn at all times.",
            **b(length_m=0.21, bill_frac=0.090, head_frac=0.140, neck_frac=0.030,
                body_frac=0.42, tail_frac=0.22, posture_deg=28,
                body_depth=0.76, body_width=0.66, chest_at=0.30, breast=0.76,
                rump=0.40, fullness=3.4, head_size=0.95, neck_up_deg=30,
                neck_thick=0.60, section=2.1, belly=0.52,
                bill_depth=0.22, bill_gape=0.12,
                tail_shape="square", tail_width=0.52, tail_droop=0.50,
                pose="perched", wing_shape="pointed", wing_span=1.85,
                wing_aspect=6.6, wing_sweep=0.40, wing_fold=0.85,
                leg_len=0.12, eye=1.0, upperparts=0.60,
                head_mark="none", wing_mark="none", body_mark="speckled",
                mark_width=0.06, mark_strength=0.26,
                mat_back="plume_iridescent", mat_belly="plume_iridescent",
                mat_head="plume_iridescent", mat_wing="plume_slate",
                mat_mark="plume_white", mat_head_mark="plume_white",
                mat_bill="skin_yellow", mat_eye="skin_dark",
                bio_grassland=0.9, bio_temperate_forest=0.6, bio_savanna=0.35,
                bio_beach=0.3,
                place_abundance=1.0, place_spacing_m=3.0,
                flock_despawn_m=110.0, flock_size_min=8, flock_size_max=400,
                flock_spread_m=40.0, flock_perch="canopy",
                flock_height_min_m=2.0, flock_height_max_m=60.0,
                flock_flight_share=0.45, flock_per_hectare=30.0)),
    ),
    "eurasian-hoopoe": (
        "the crest and the decurved bill: banded wings, sandy body",
        base(
            name="eurasian-hoopoe",
            notes="Two extremes in one bird and the reason both sliders exist: "
                  "the largest crest in the set at 0.9, and a bill that is "
                  "genuinely DECURVED rather than merely pointed downward. It "
                  "is also the desert and savanna species, and its sandy body "
                  "with hard black-and-white bars is what the barred body "
                  "marking was written for.",
            **b(length_m=0.27, bill_frac=0.160, head_frac=0.115, neck_frac=0.045,
                body_frac=0.38, tail_frac=0.32, posture_deg=20,
                body_depth=0.80, body_width=0.66, chest_at=0.34, breast=0.70,
                rump=0.46, fullness=3.0, head_size=0.95, neck_up_deg=26,
                neck_thick=0.50, section=2.1, belly=0.52, crest=0.90,
                bill_depth=0.16, bill_curve=0.55, bill_gape=0.05,
                tail_shape="square", tail_width=0.50, tail_droop=0.50,
                pose="perched", wing_shape="elliptical", wing_span=1.85,
                wing_aspect=5.0, wing_fold=0.55,
                leg_len=0.11, eye=1.0, upperparts=0.44,
                head_mark="none", wing_mark="doublebar", body_mark="none",
                mark_width=0.22,
                mat_back="plume_buff", mat_belly="plume_white",
                mat_head="plume_buff", mat_wing="plume_white",
                mat_mark="skin_dark", mat_head_mark="skin_dark",
                mat_bill="beak_horn", mat_eye="skin_dark",
                bio_savanna=0.8, bio_desert=0.6, bio_grassland=0.4,
                place_abundance=0.3, place_spacing_m=80.0,
                flock_despawn_m=80.0, flock_size_min=1, flock_size_max=2,
                flock_spread_m=15.0, flock_perch="ground",
                flock_height_min_m=1.0, flock_height_max_m=12.0,
                flock_flight_share=0.25, flock_per_hectare=0.6)),
    ),
    "great-spotted-woodpecker": (
        "pied, vertical on a trunk, chisel bill, crimson nape",
        base(
            name="great-spotted-woodpecker",
            notes="The most extreme POSTURE in the set at 68 degrees: this is "
                  "a bird drawn clinging to a trunk, and it is a different "
                  "silhouette from every other perching species here for that "
                  "reason alone. Also the species that forced the head to get "
                  "its own marking colour -- it is white-panelled on the wing "
                  "and CRIMSON on the nape, and one shared marking colour made "
                  "it choose one or the other.\n\n"
                  "The stiff tail props it against the trunk, so the tail "
                  "carriage is 0.80, which is nearly straight out of the body "
                  "line rather than hanging.",
            **b(length_m=0.23, bill_frac=0.105, head_frac=0.130, neck_frac=0.030,
                body_frac=0.42, tail_frac=0.315, posture_deg=68,
                body_depth=0.72, body_width=0.66, chest_at=0.30, breast=0.74,
                rump=0.44, fullness=3.2, head_size=1.0, neck_up_deg=-6,
                neck_thick=0.70, section=2.2, belly=0.50,
                bill_depth=0.26, bill_gape=0.16,
                tail_shape="pointed", tail_width=0.44, tail_droop=0.80,
                tail_thick=2,
                pose="perched", wing_shape="elliptical", wing_span=1.60,
                wing_aspect=5.2, wing_fold=0.40,
                leg_len=0.09, eye=1.0, upperparts=0.62,
                head_mark="cap", wing_mark="panel", body_mark="none",
                mark_width=0.20,
                mat_back="skin_dark", mat_belly="plume_white",
                mat_head="skin_dark", mat_wing="skin_dark",
                mat_mark="plume_white", mat_head_mark="plume_crimson",
                mat_bill="plume_grey", mat_eye="plume_crimson",
                bio_temperate_forest=0.85, bio_taiga=0.6,
                place_abundance=0.35, place_spacing_m=60.0,
                flock_despawn_m=70.0, flock_size_min=1, flock_size_max=1,
                flock_spread_m=4.0, flock_perch="canopy",
                flock_height_min_m=2.0, flock_height_max_m=20.0,
                flock_flight_share=0.18, flock_per_hectare=0.8)),
    ),
    # --- raptors and owls ---------------------------------------------------
    "tawny-owl": (
        "all head and no neck, mottled brown, huge dark eyes",
        base(
            name="tawny-owl",
            notes="The head-size extreme at 1.42, and the only species here "
                  "with essentially no neck: the head and body run together, "
                  "which is what makes an owl an owl at ten voxels. Two-voxel "
                  "eyes, which nothing else in the set has and which cost four "
                  "voxels for most of the recognition.",
            **b(length_m=0.40, bill_frac=0.045, head_frac=0.185, neck_frac=0.010,
                body_frac=0.475, tail_frac=0.285, posture_deg=46,
                body_depth=0.74, body_width=0.80, chest_at=0.30, breast=0.80,
                rump=0.52, fullness=3.4, head_size=1.42, neck_up_deg=44,
                neck_thick=1.10, section=2.4, belly=0.52,
                bill_depth=0.55, bill_hook=0.70, bill_gape=0.20,
                tail_shape="rounded", tail_width=0.58, tail_droop=0.35,
                tail_thick=2,
                pose="perched", wing_shape="elliptical", wing_span=2.45,
                wing_aspect=5.3, wing_fold=0.60, wing_thick=2,
                leg_len=0.08, eye=2.0, upperparts=0.55,
                head_mark="mask", wing_mark="none", body_mark="streaked",
                mark_count=7, mark_width=0.30, mark_strength=0.30,
                mat_back="skin_brown", mat_belly="plume_buff",
                mat_head="plume_buff", mat_wing="skin_brown",
                mat_mark="skin_dark", mat_head_mark="skin_brown",
                mat_bill="beak_horn", mat_eye="skin_dark",
                bio_temperate_forest=0.7, bio_taiga=0.35,
                place_abundance=0.15, place_spacing_m=200.0,
                flock_despawn_m=90.0, flock_size_min=1, flock_size_max=1,
                flock_spread_m=4.0, flock_perch="canopy",
                flock_height_min_m=2.0, flock_height_max_m=25.0,
                flock_flight_share=0.15, flock_per_hectare=0.15)),
    ),
    "golden-eagle": (
        "slotted wings with six fingers, hooked bill, golden nape",
        base(
            name="golden-eagle",
            notes="The SLOTTED wing, and the biggest asset in the set: a 2.1 m "
                  "span at 1 cm is 210 voxels across. Six separated finger "
                  "feathers at the tip, which is real geometry with daylight "
                  "between it and the most legible thing about a big soaring "
                  "bird seen from below.\n\n"
                  "Authored FLYING, because that is where an eagle is seen and "
                  "because the wing planform is invisible folded. Its aspect "
                  "ratio of 6.9 is measured (Alerstam et al. 2007, 129 species "
                  "from 33,610 individual measurements) and it is the number "
                  "that separates it from an albatross: a soaring eagle gets "
                  "its low wing loading from AREA, not from length.",
            **b(length_m=0.85, bill_frac=0.055, head_frac=0.115, neck_frac=0.055,
                body_frac=0.395, tail_frac=0.38, posture_deg=4,
                body_depth=0.66, body_width=0.72, chest_at=0.32, breast=0.74,
                rump=0.46, fullness=3.0, head_size=1.0, neck_up_deg=12,
                neck_thick=0.62, section=2.2, belly=0.50,
                bill_depth=0.52, bill_hook=0.85, bill_gape=0.20,
                tail_shape="rounded", tail_width=0.56, tail_droop=0.40,
                tail_thick=3,
                pose="flying", wing_shape="slotted", wing_span=2.45,
                wing_aspect=6.9, wing_sweep=0.16, wing_dihedral=0.10,
                wing_slots=6, wing_thick=3, wing_fold=0.70,
                leg_len=0.09, eye=1.0, upperparts=0.58,
                head_mark="cap", wing_mark="panel", body_mark="none",
                mark_width=0.62,
                mat_back="skin_dark", mat_belly="skin_brown",
                mat_head="skin_brown", mat_wing="skin_dark",
                mat_mark="plume_buff", mat_head_mark="skin_yellow",
                mat_bill="skin_yellow", mat_eye="skin_dark",
                bio_tundra_alpine=0.85, bio_bare_rock=0.7, bio_taiga=0.4,
                bio_grassland=0.25,
                place_abundance=0.06, place_spacing_m=1500.0,
                place_slope_max_pct=70.0,
                flock_despawn_m=600.0, flock_size_min=1, flock_size_max=2,
                flock_spread_m=120.0, flock_perch="cliff",
                flock_height_min_m=40.0, flock_height_max_m=600.0,
                flock_flight_share=0.85, flock_per_hectare=0.02)),
    ),
    "common-buzzard": (
        "the broad soaring one: short fingered wings, fanned tail",
        base(
            name="common-buzzard",
            notes="An eagle's wing at half the size, and the LOWEST aspect "
                  "ratio of any raptor measured -- 5.6, against a falcon's 7.9 "
                  "and an accipiter's 6.2. That ordering is the whole "
                  "difference between the three raptor shapes here and it is "
                  "the reason `wing_aspect` is a slider rather than a "
                  "consequence of the planform choice.",
            **b(length_m=0.52, bill_frac=0.050, head_frac=0.115, neck_frac=0.045,
                body_frac=0.40, tail_frac=0.39, posture_deg=4,
                body_depth=0.70, body_width=0.74, chest_at=0.32, breast=0.76,
                rump=0.48, fullness=3.1, head_size=1.0, neck_up_deg=12,
                neck_thick=0.60, section=2.2, belly=0.50,
                bill_depth=0.48, bill_hook=0.72, bill_gape=0.18,
                tail_shape="rounded", tail_width=0.62, tail_droop=0.40,
                tail_thick=2,
                pose="flying", wing_shape="slotted", wing_span=2.35,
                wing_aspect=5.6, wing_sweep=0.12, wing_dihedral=0.18,
                wing_slots=4, wing_thick=2, wing_fold=0.60,
                leg_len=0.10, eye=1.0, upperparts=0.60,
                head_mark="none", wing_mark="panel", body_mark="none",
                mark_width=0.30,
                mat_back="skin_brown", mat_belly="plume_white",
                mat_head="skin_brown", mat_wing="skin_brown",
                mat_mark="plume_white", mat_head_mark="plume_buff",
                mat_bill="skin_yellow", mat_eye="skin_dark",
                bio_temperate_forest=0.7, bio_grassland=0.7, bio_taiga=0.35,
                bio_bare_rock=0.3,
                place_abundance=0.12, place_spacing_m=600.0,
                place_slope_max_pct=70.0,
                flock_despawn_m=350.0, flock_size_min=1, flock_size_max=3,
                flock_spread_m=80.0, flock_perch="canopy",
                flock_height_min_m=20.0, flock_height_max_m=300.0,
                flock_flight_share=0.70, flock_per_hectare=0.08)),
    ),
    "common-kestrel": (
        "pointed falcon wings, rufous back, slate head, black tail band",
        base(
            name="common-kestrel",
            notes="The POINTED planform, and the third of the three raptor "
                  "shapes. Its wings taper to a point from halfway out and "
                  "rake back; a buzzard's are broad and fingered and an owl's "
                  "are round. All three are the same four sliders.",
            **b(length_m=0.34, bill_frac=0.045, head_frac=0.115, neck_frac=0.040,
                body_frac=0.38, tail_frac=0.42, posture_deg=8,
                body_depth=0.66, body_width=0.70, chest_at=0.32, breast=0.74,
                rump=0.44, fullness=3.1, head_size=1.0, neck_up_deg=16,
                neck_thick=0.58, section=2.1, belly=0.50,
                bill_depth=0.55, bill_hook=0.60, bill_gape=0.16,
                tail_shape="rounded", tail_width=0.40, tail_droop=0.45,
                pose="flying", wing_shape="pointed", wing_span=2.20,
                wing_aspect=7.5, wing_sweep=0.30, wing_dihedral=-0.05,
                leg_len=0.10, eye=1.0, upperparts=0.56, wing_fold=0.90,
                head_mark="none", wing_mark="tip", body_mark="speckled",
                mark_width=0.07, mark_strength=0.22,
                mat_back="plume_rufous", mat_belly="plume_buff",
                mat_head="plume_slate", mat_wing="plume_rufous",
                mat_mark="skin_dark", mat_head_mark="skin_dark",
                mat_bill="skin_yellow", mat_eye="skin_dark",
                bio_grassland=0.9, bio_savanna=0.7, bio_desert=0.35,
                bio_bare_rock=0.3, bio_beach=0.25,
                place_abundance=0.2, place_spacing_m=300.0,
                place_slope_max_pct=70.0,
                flock_despawn_m=200.0, flock_size_min=1, flock_size_max=1,
                flock_spread_m=20.0, flock_perch="cliff",
                flock_height_min_m=8.0, flock_height_max_m=90.0,
                flock_flight_share=0.65, flock_per_hectare=0.15)),
    ),
    # --- aerial -------------------------------------------------------------
    "barn-swallow": (
        "deep forked tail, swept pointed wings, rufous throat",
        base(
            name="barn-swallow",
            notes="The FORK, and the measured one: a male barn swallow's fork "
                  "depth is 52% of its tail length (Nam et al. 2018, 82 males, "
                  "45.8 +/- 7.7 mm on an 88 mm tail). That is the number "
                  "`tail_fork` is set to and it is what makes the silhouette "
                  "unmistakable even at ten voxels.\n\n"
                  "AUTHORED AT 26 cm against a real 17-19, so the fork has "
                  "voxels to be a fork in. `tools/birdprobe.py --read` flagged "
                  "it SHORT at 22 and passes it at 26.",
            **b(length_m=0.26, bill_frac=0.045, head_frac=0.135, neck_frac=0.025,
                body_frac=0.375, tail_frac=0.42, posture_deg=8,
                body_depth=0.62, body_width=0.68, chest_at=0.28, breast=0.74,
                rump=0.34, fullness=3.4, head_size=1.05, neck_up_deg=16,
                neck_thick=0.66, section=2.0, belly=0.50,
                bill_depth=0.16, bill_gape=0.60,
                tail_shape="forked", tail_width=0.62, tail_fork=0.52,
                tail_droop=0.40,
                pose="flying", wing_shape="pointed", wing_span=1.95,
                wing_aspect=7.5, wing_sweep=0.62, wing_dihedral=0.06,
                wing_fold=1.15,
                leg_len=0.04, eye=1.0, upperparts=0.54,
                head_mark="throat", wing_mark="none", body_mark="none",
                mark_width=0.44,
                mat_back="plume_iridescent", mat_belly="plume_white",
                mat_head="plume_iridescent", mat_wing="plume_iridescent",
                mat_mark="plume_white", mat_head_mark="skin_orange",
                mat_bill="skin_yellow", mat_eye="skin_dark",
                bio_grassland=0.9, bio_savanna=0.55, bio_beach=0.5,
                bio_temperate_forest=0.4,
                place_abundance=0.8, place_spacing_m=6.0,
                flock_despawn_m=140.0, flock_size_min=4, flock_size_max=40,
                flock_spread_m=50.0, flock_perch="shrub",
                flock_height_min_m=1.0, flock_height_max_m=40.0,
                flock_flight_share=0.90, flock_per_hectare=10.0)),
    ),
    "herring-gull": (
        "high-aspect plank wings, grey mantle, black wingtips",
        base(
            name="herring-gull",
            notes="The SOARING planform: a long narrow plank of nearly "
                  "constant chord, aspect ratio 9.7 against an eagle's 6.9. "
                  "Its wingtips are black and that is the most reliable gull "
                  "mark there is -- measured across 50 gull species, the ratio "
                  "of black to non-black at the wingtip rises by a factor of "
                  "1.4 for each standard deviation of wing loading (Goumas "
                  "2022). It is also the only species here whose whole "
                  "identity is TWO greys and one black.",
            **b(length_m=0.60, bill_frac=0.075, head_frac=0.105, neck_frac=0.070,
                body_frac=0.42, tail_frac=0.33, posture_deg=4,
                body_depth=0.62, body_width=0.78, chest_at=0.32, breast=0.76,
                rump=0.42, fullness=3.2, head_size=0.95, neck_up_deg=14,
                neck_thick=0.60, section=2.2, belly=0.50,
                bill_depth=0.30, bill_hook=0.28, bill_gape=0.14,
                tail_shape="square", tail_width=0.60, tail_droop=0.40,
                tail_thick=2,
                pose="flying", wing_shape="soaring", wing_span=2.35,
                wing_aspect=9.7, wing_sweep=0.20, wing_dihedral=0.06,
                wing_thick=2, wing_fold=0.95,
                leg_len=0.14, eye=1.0, upperparts=0.42,
                head_mark="none", wing_mark="tip", body_mark="none",
                mark_width=0.16,
                mat_back="plume_grey", mat_belly="plume_white",
                mat_head="plume_white", mat_wing="plume_grey",
                mat_mark="skin_dark", mat_head_mark="plume_grey",
                mat_bill="skin_orange", mat_eye="plume_white",
                bio_beach=0.95, bio_ocean=0.8, bio_bare_rock=0.35,
                bio_grassland=0.3,
                place_abundance=0.7, place_spacing_m=15.0,
                place_slope_max_pct=70.0,
                flock_despawn_m=300.0, flock_size_min=3, flock_size_max=60,
                flock_spread_m=90.0, flock_perch="cliff",
                flock_height_min_m=3.0, flock_height_max_m=150.0,
                flock_flight_share=0.75, flock_per_hectare=3.0)),
    ),
    # --- waterside ----------------------------------------------------------
    "grey-heron": (
        "all neck and legs, dagger bill, slate wings",
        base(
            name="grey-heron",
            notes="The PROPORTION extreme, and the species the neck and leg "
                  "sliders exist for: a quarter of its length is neck and a "
                  "third of it is leg, against 0.03 and 0.04 on a swallow. "
                  "Measured on AVONET family medians, herons carry a tarsus a "
                  "third of their wing chord and cranes 45%, against 8% for a "
                  "swift -- a five-fold spread that no other cue matches.\n\n"
                  "The dagger bill is a separate extreme: bill width near zero "
                  "where a mallard's is near one, on bills of the same length.",
            **b(length_m=1.00, bill_frac=0.150, head_frac=0.075, neck_frac=0.260,
                body_frac=0.375, tail_frac=0.140, posture_deg=6,
                body_depth=0.70, body_width=0.64, chest_at=0.34, breast=0.66,
                rump=0.46, fullness=3.0, head_size=0.85, neck_up_deg=64,
                neck_thick=0.26, section=2.1, belly=0.50,
                bill_depth=0.20, bill_gape=0.03,
                tail_shape="square", tail_width=0.70, tail_droop=0.30,
                tail_thick=2,
                pose="perched", wing_shape="slotted", wing_span=1.85,
                wing_aspect=7.2, wing_slots=4, wing_thick=2, wing_fold=0.85,
                leg_len=0.30, leg_thick=1.5, eye=1.0, upperparts=0.50,
                head_mark="cap", wing_mark="none", body_mark="streaked",
                mark_count=5, mark_width=0.24,
                mat_back="plume_grey", mat_belly="plume_white",
                mat_head="plume_white", mat_wing="plume_slate",
                mat_mark="skin_dark", mat_head_mark="skin_dark",
                mat_bill="skin_orange", mat_eye="skin_yellow",
                bio_temperate_forest=0.7, bio_beach=0.6, bio_grassland=0.5,
                bio_rainforest=0.3, bio_savanna=0.3,
                place_abundance=0.15, place_spacing_m=150.0,
                place_water_max_m=60.0,
                flock_despawn_m=220.0, flock_size_min=1, flock_size_max=3,
                flock_spread_m=40.0, flock_perch="waterside",
                flock_height_min_m=5.0, flock_height_max_m=80.0,
                flock_flight_share=0.20, flock_per_hectare=0.2)),
    ),
    "common-kingfisher": (
        "turquoise, all bill and head, almost no tail",
        base(
            name="common-kingfisher",
            notes="The BILL extreme, and the shortest tail in the set. Across "
                  "AVONET's family medians kingfishers carry the longest bill "
                  "relative to wing chord of any family measured -- 0.50, "
                  "against 0.21 for a corvid -- on the shortest tarsus of any "
                  "waterside bird. It is a bill and a head with a bird behind "
                  "it, and that is the whole silhouette.\n\n"
                  "AUTHORED AT 20 cm against a real 17. COLOUR PUSHED HARD: a "
                  "real kingfisher photographs as a deep cobalt that only "
                  "flares turquoise at one angle, because the colour is "
                  "structural rather than pigmented. Structural colour is 7% of "
                  "the plumage on a bird and 45% of the gamut; drawn at its "
                  "average it disappears, so this one is drawn at its flare.",
            **b(length_m=0.20, bill_frac=0.200, head_frac=0.150, neck_frac=0.020,
                body_frac=0.415, tail_frac=0.115, posture_deg=26,
                body_depth=0.80, body_width=0.72, chest_at=0.30, breast=0.80,
                rump=0.50, fullness=3.6, head_size=1.35, neck_up_deg=24,
                neck_thick=0.90, section=2.2, belly=0.52,
                bill_depth=0.24, bill_gape=0.05,
                tail_shape="square", tail_width=0.70, tail_droop=0.50,
                pose="perched", wing_shape="elliptical", wing_span=1.70,
                wing_aspect=5.0, wing_fold=0.90,
                leg_len=0.055, eye=1.0, upperparts=0.56,
                head_mark="mask", wing_mark="none", body_mark="none",
                mark_width=0.24,
                mat_back="plume_cyan", mat_belly="plume_rufous",
                mat_head="plume_cyan", mat_wing="plume_cyan",
                mat_mark="plume_white", mat_head_mark="plume_white",
                mat_bill="skin_dark", mat_eye="skin_dark",
                bio_temperate_forest=0.7, bio_rainforest=0.6,
                bio_grassland=0.35,
                place_abundance=0.25, place_spacing_m=120.0,
                place_water_max_m=15.0,
                flock_despawn_m=70.0, flock_size_min=1, flock_size_max=1,
                flock_spread_m=6.0, flock_perch="waterside",
                flock_height_min_m=0.5, flock_height_max_m=6.0,
                flock_flight_share=0.20, flock_per_hectare=0.4)),
    ),
    "mallard-duck": (
        "level body, spatulate bill, green head, white collar, cyan speculum",
        base(
            name="mallard-duck",
            notes="The SPATULA, and the reason bill width is a slider: a "
                  "mallard's bill and a heron's are almost the same length and "
                  "nobody confuses them. Across AVONET's trophic niches "
                  "aquatic herbivores are the only group whose bills are wider "
                  "than they are deep -- width:depth 1.17, against 0.73 for an "
                  "aquatic predator.\n\n"
                  "Also the flattest posture in the set at 4 degrees, and the "
                  "shortest tail-to-body of any perching species, which "
                  "together are what a duck IS in silhouette.",
            **b(length_m=0.58, bill_frac=0.115, head_frac=0.110, neck_frac=0.100,
                body_frac=0.475, tail_frac=0.200, posture_deg=4,
                body_depth=0.64, body_width=0.92, chest_at=0.34, breast=0.72,
                rump=0.52, fullness=3.0, head_size=1.05, neck_up_deg=34,
                neck_thick=0.62, section=2.6, belly=0.56,
                bill_depth=0.28, bill_gape=0.95,
                tail_shape="pointed", tail_width=0.52, tail_droop=0.20,
                pose="perched", wing_shape="pointed", wing_span=1.55,
                wing_aspect=9.2, wing_sweep=0.35, wing_thick=2, wing_fold=0.70,
                leg_len=0.075, eye=1.0, upperparts=0.52,
                head_mark="collar", wing_mark="panel", body_mark="none",
                mark_width=0.24,
                mat_back="plume_grey", mat_belly="plume_grey",
                mat_head="plume_iridescent", mat_wing="plume_slate",
                mat_mark="plume_cyan", mat_head_mark="plume_white",
                mat_bill="skin_yellow", mat_eye="skin_dark",
                bio_grassland=0.8, bio_temperate_forest=0.7, bio_beach=0.5,
                bio_taiga=0.4, bio_savanna=0.25,
                place_abundance=0.55, place_spacing_m=10.0,
                place_water_max_m=25.0,
                flock_despawn_m=150.0, flock_size_min=2, flock_size_max=20,
                flock_spread_m=25.0, flock_perch="water",
                flock_height_min_m=3.0, flock_height_max_m=80.0,
                flock_flight_share=0.20, flock_per_hectare=4.0)),
    ),
    # --- open ground and rock ----------------------------------------------
    "rock-pigeon": (
        "grey, square-tailed, two black wing bars, lilac neck",
        base(
            name="rock-pigeon",
            notes="The plain one, and the test of whether two greys and a "
                  "marking are enough. Its shape carries almost nothing "
                  "distinctive -- a small head on a heavy body with a square "
                  "tail -- so its identity is entirely the DOUBLE WING BAR and "
                  "the neck.\n\n"
                  "COLOUR PUSHED. A real pigeon's neck gloss is a shifting "
                  "green-purple that only fires at one angle. Here it is a flat "
                  "violet collar, which is the only violet in the library and "
                  "the reason `plume_lilac` is proposed at all.",
            **b(length_m=0.33, bill_frac=0.055, head_frac=0.115, neck_frac=0.055,
                body_frac=0.43, tail_frac=0.345, posture_deg=16,
                body_depth=0.80, body_width=0.74, chest_at=0.30, breast=0.80,
                rump=0.48, fullness=3.2, head_size=0.85, neck_up_deg=32,
                neck_thick=0.58, section=2.3, belly=0.54,
                bill_depth=0.30, bill_gape=0.18,
                tail_shape="square", tail_width=0.50, tail_droop=0.45,
                pose="perched", wing_shape="pointed", wing_span=1.85,
                wing_aspect=8.6, wing_sweep=0.30, wing_fold=0.75,
                leg_len=0.095, eye=1.0, upperparts=0.50,
                head_mark="collar", wing_mark="doublebar", body_mark="none",
                mark_width=0.18,
                mat_back="plume_grey", mat_belly="plume_grey",
                mat_head="plume_slate", mat_wing="plume_grey",
                mat_mark="skin_dark", mat_head_mark="plume_lilac",
                mat_bill="skin_orange", mat_eye="skin_orange",
                bio_bare_rock=0.8, bio_grassland=0.7, bio_beach=0.5,
                bio_desert=0.3, bio_savanna=0.25,
                place_abundance=0.7, place_spacing_m=8.0,
                place_slope_max_pct=70.0,
                flock_despawn_m=120.0, flock_size_min=3, flock_size_max=40,
                flock_spread_m=30.0, flock_perch="cliff",
                flock_height_min_m=2.0, flock_height_max_m=60.0,
                flock_flight_share=0.35, flock_per_hectare=8.0)),
    ),
    "rock-ptarmigan": (
        "the white gamebird: plump, short rounded wings, red comb",
        base(
            name="rock-ptarmigan",
            notes="The GAMEBIRD body -- plump, low, short-legged, with the "
                  "roundest and lowest-aspect wing in the set at 4.6 -- and "
                  "the only species that is essentially one colour. In winter "
                  "plumage it is white with a black eye-stripe and a crimson "
                  "comb, and those two marks on nine hundred white voxels are "
                  "the whole identity.\n\n"
                  "It is here to prove the value-contrast gate does something: "
                  "white on white would be invisible, and the two marks are "
                  "the only two things that are not.",
            **b(length_m=0.36, bill_frac=0.045, head_frac=0.115, neck_frac=0.045,
                body_frac=0.52, tail_frac=0.275, posture_deg=14,
                body_depth=0.86, body_width=0.84, chest_at=0.32, breast=0.78,
                rump=0.52, fullness=3.4, head_size=0.90, neck_up_deg=30,
                neck_thick=0.60, section=2.5, belly=0.56,
                bill_depth=0.42, bill_curve=0.20, bill_gape=0.22,
                tail_shape="square", tail_width=0.52, tail_droop=0.35,
                pose="perched", wing_shape="elliptical", wing_span=1.35,
                wing_aspect=4.6, wing_fold=0.55,
                leg_len=0.075, eye=1.0, upperparts=0.50,
                head_mark="supercilium", wing_mark="tip", body_mark="none",
                mark_width=0.26,
                mat_back="plume_white", mat_belly="plume_white",
                mat_head="plume_white", mat_wing="plume_grey",
                mat_mark="skin_dark", mat_head_mark="plume_crimson",
                mat_bill="skin_dark", mat_eye="skin_dark",
                bio_tundra_alpine=0.9, bio_bare_rock=0.5, bio_taiga=0.3,
                place_abundance=0.3, place_spacing_m=60.0,
                place_slope_max_pct=70.0,
                flock_despawn_m=80.0, flock_size_min=2, flock_size_max=8,
                flock_spread_m=15.0, flock_perch="ground",
                flock_height_min_m=0.5, flock_height_max_m=8.0,
                flock_flight_share=0.08, flock_per_hectare=1.0)),
    ),
    # --- rainforest ---------------------------------------------------------
    "scarlet-macaw": (
        "the loudest thing in the library: crimson, long graduated tail",
        base(
            name="scarlet-macaw",
            notes="The TAIL extreme at 0.55 of the total length, and the "
                  "biggest bill in the set: parrots are the stubbiest-billed "
                  "family measured, with a bill depth-to-length median of 0.85 "
                  "against a hummingbird's 0.09, and the deepest of them are "
                  "genuinely deeper than they are long.\n\n"
                  "It is also the answer to what the palette is FOR. Three "
                  "saturated colours on one animal -- crimson body, blue wing, "
                  "yellow panel -- with a white bare face. Nothing else in the "
                  "library needs that and nothing else in the world looks like "
                  "it.",
            **b(length_m=0.85, bill_frac=0.070, head_frac=0.110, neck_frac=0.030,
                body_frac=0.245, tail_frac=0.545, posture_deg=30,
                body_depth=0.88, body_width=0.70, chest_at=0.32, breast=0.74,
                rump=0.42, fullness=3.4, head_size=1.15, neck_up_deg=34,
                neck_thick=0.62, section=2.1, belly=0.52,
                bill_depth=0.85, bill_curve=0.30, bill_hook=0.80,
                bill_gape=0.25,
                tail_shape="graduated", tail_width=0.20, tail_droop=0.60,
                tail_thick=2,
                pose="perched", wing_shape="pointed", wing_span=1.30,
                wing_aspect=6.5, wing_sweep=0.30, wing_thick=2, wing_fold=0.45,
                leg_len=0.075, eye=1.0, upperparts=0.55,
                head_mark="mask", wing_mark="panel", body_mark="none",
                mark_width=0.30,
                mat_back="plume_crimson", mat_belly="plume_crimson",
                mat_head="plume_crimson", mat_wing="skin_blue",
                mat_mark="skin_yellow", mat_head_mark="plume_white",
                mat_bill="plume_white", mat_eye="skin_dark",
                bio_rainforest=0.95,
                place_abundance=0.25, place_spacing_m=80.0,
                flock_despawn_m=160.0, flock_size_min=2, flock_size_max=8,
                flock_spread_m=30.0, flock_perch="canopy",
                flock_height_min_m=10.0, flock_height_max_m=60.0,
                flock_flight_share=0.30, flock_per_hectare=0.5)),
    ),
}


def main() -> int:
    force = seedspec.parse_force(sys.argv[1:])
    seedspec.announce(force, "bird specs")
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

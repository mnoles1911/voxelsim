"""Re-author every tree crown on the measured envelope model.

What changed under these species is the crown itself: the profile is now the
two-sided Lamé form foresters actually fit to scanned trees, and there are two
new axes that no amount of tuning the old parameters could reach -- how
lopsided a crown is seen from above, and how far it has slid off its own trunk.
Both are things every real tree has and no surface of revolution can express.

So the crown block of each species is rewritten here rather than nudged. What
is NOT touched is biome weights, placement, height, materials or growth model:
those encode decisions about where a species belongs in the world, which have
nothing to do with the shape of its crown and were not what the model changed.

The three numbers per species come from measurement where measurement exists:

    offset      forest-grown crowns sit about 0.37 of their own radius off
                their stem as they lean away from neighbours; isolated
                open-grown trees manage roughly half that. So a hedgerow elm
                and a specimen cherry get low values and anything grown in a
                stand gets high ones.
    asymmetry   spread between the radii measured around one trunk. It widens
                with age, and small trees are close to symmetric -- which is
                why the sapling and the clipped cypress are near zero and the
                wind-sheared krummholz is near the top.
    top_bias    leaf density rises from crown base to crown top in every
                species measured. Strongest where the crown is a shallow layer
                over open air, as on a jungle emergent; weakest on a willow,
                whose foliage genuinely does hang below its own crown.
"""
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"

#     shell_up / shell_low
#                 how deep the leafy layer goes in from the crown surface,
#                 above and below the widest point, as a fraction of the local
#                 radius. Broadleaves run thinner below than above, because
#                 nothing in a shaded lower crown gets the light to hold
#                 leaves. The willow is the one that inverts -- its foliage
#                 genuinely does hang below the crown -- and being able to say
#                 that at all is the reason this replaced a single symmetric
#                 hollowness number.

# name: (shape, asymmetry, offset, top_bias, shell_up, shell_low)
CROWNS = {
    # wind-sheared at altitude: the most lopsided thing in the library, and
    # pushed hard downwind rather than merely uneven.
    "alpine-krummholz":   ("wedge",    0.70, 0.50, 0.45, 0.80, 0.70),
    # a specimen with nothing near it: broad, even, barely displaced.
    "baobab":             ("umbrella", 0.35, 0.14, 0.25, 0.50, 0.20),
    # slender and grown in a stand, so it reaches sideways for light.
    "birch":              ("column",   0.30, 0.32, 0.50, 0.45, 0.30),
    "cherry-blossom":     ("sphere",   0.25, 0.15, 0.30, 0.75, 0.55),
    # fronds are placed by their own growth model; the envelope barely applies.
    "coast-palm":         ("umbrella", 0.20, 0.10, 0.00, 0.60, 0.40),
    # clipped and formal. Near-symmetric on purpose -- this is the one species
    # where a surface of revolution is the right answer.
    "columnar-cypress":   ("column",   0.10, 0.06, 0.30, 0.85, 0.80),
    # dead, so no foliage gradient to speak of, but a ragged outline.
    "desert-dead":        ("vase",     0.50, 0.30, 0.00, 0.50, 0.40),
    # hedgerow tree, open on one side and crowded on the other.
    "field-elm":          ("vase",     0.40, 0.24, 0.40, 0.60, 0.35),
    # browsed and knocked about; scrub is lopsided almost by definition.
    "hawthorn-scrub":     ("sphere",   0.55, 0.30, 0.35, 0.80, 0.60),
    # stands above the canopy, so its crown is a shallow layer over open air
    # and its leaves are concentrated hard at the top.
    "jungle-emergent":    ("umbrella", 0.40, 0.26, 0.65, 0.45, 0.12),
    # mature broadleaf: carries its width high, grown among others.
    "river-broadleaf":    ("ovoid",    0.35, 0.35, 0.45, 0.65, 0.40),
    # open-grown, flat-topped, and top-heavy because everything below the
    # canopy layer is browsed off.
    "savanna-acacia":     ("umbrella", 0.35, 0.18, 0.50, 0.40, 0.15),
    # the measured old-growth case, at the measured displacement.
    "temperate-oak":      ("vase",     0.40, 0.37, 0.45, 0.60, 0.35),
    # young trees measure as near-symmetric; that is a real finding, not a
    # missing feature.
    "temperate-sapling":  ("sphere",   0.12, 0.10, 0.25, 0.85, 0.70),
    # narrow high-latitude conifer: the tighter of the two measured conifer
    # profiles, widest only a sixth of the way up.
    "tundra-pine":        ("spire",    0.30, 0.20, 0.40, 0.55, 0.50),
    # foliage really does hang below the crown here, so the top bias that is
    # right everywhere else is wrong on this one.
    "weeping-willow":     ("hanging",  0.30, 0.25, 0.15, 0.50, 0.75),
}


def main():
    changed = 0
    for name, (shape, asym, offset, top, up, low) in sorted(CROWNS.items()):
        path = SPECS / f"{name}.json"
        if not path.exists():
            print(f"  {name:<20} MISSING")
            continue
        spec, _ = sm.load(path)
        before = sm.get(spec, "crown.shape")
        out, rep = sm.patch(spec, {
            "crown.shape": shape,
            "crown.asymmetry": asym,
            "crown.offset": offset,
            "foliage.top_bias": top,
            "crown.shell_upper": up,
            "crown.shell_lower": low,
        })
        sm.save(out, path)
        changed += 1
        note = "" if before == shape else f"   shape {before} -> {shape}"
        warn = ("  ! " + "; ".join(rep.warnings)) if rep.warnings else ""
        print(f"  {name:<20} asym {asym:.2f}  offset {offset:.2f}  "
              f"top {top:.2f}  shell {up:.2f}/{low:.2f}{note}{warn}")
    print(f"\n{changed} tree species re-authored on the measured crown model.")


if __name__ == "__main__":
    main()

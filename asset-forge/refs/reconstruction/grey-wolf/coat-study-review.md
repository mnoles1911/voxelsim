# Grey wolf coat study — September 7, 2026

The user supported continuing this source-based workflow. This iteration keeps
the same 101,602 occupied 12.5 mm cells and changes appearance only, allowing a
direct comparison without confusing shape and color improvements.

Run `python tools/refine_wolf_pilot.py` from Asset Forge after the source study.
Output: `out/creature-reconstruction/grey-wolf-coat-study/`.

## Changes and review

- A dark dorsal coat blends into warm gray/buff flanks and lighter legs/cheeks,
  informed by the credited NPS Canyon alpha photograph in this directory.
- Neighborhood filtering reduces isolated texture noise while retaining some
  original fur luminance variation. The first revision was too smooth; the
  second retains more grizzling and is the current output.
- Source cyan eye pixels become amber-brown; nose and mouth details remain.
  White claw pixels are subdued. Eye and paw geometry has not been corrected.
- Six orthographic views show a clearer separation of coat regions. This is a
  useful appearance candidate, not a photometrically calibrated reconstruction
  or proof of anatomical correctness. Coat boundaries still need refinement.

The authored colors and region parameters describe this study, not universal
wolf markings. The source mesh attribution remains in `mesh-source-reviews.json`.
Its original CC BY credit applies to derived outputs.

## Anatomical scale finding

The source was previously normalized to 1.81 m total bounding length. In the
manually selected x=0.60–0.75 m shoulder region, its ground-to-coat envelope is
about 1.025 m. This includes fur and is not a measured skeletal landmark.
[NPS](https://www.nps.gov/yell/learn/nature/wolf.htm) lists 81 cm average male
shoulder height, 77 cm for females, and a general 26–36 inch shoulder range.
The discrepancy warrants reviewing the source's body proportions and landmark
placement. Do not independently force population averages onto every dimension
or flatten the whole animal merely to make one number pass.

Next: establish explicit withers, elbow, wrist, hock and paw landmarks on the
continuous source mesh, compare head/body/leg ratios to compatible reference
poses, and then regenerate at 12.5 mm. Keep the current coat study as the color
baseline during that geometry pass. Engine RGB storage and rigging remain open.

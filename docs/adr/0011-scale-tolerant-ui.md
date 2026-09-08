# ADR-0011: Scale-tolerant UI — one 1080p reference, continuous scaling, no pixel-locked primitives

- **Status:** accepted
- **Date:** 2026-09-07
- **Doctrine sections affected:** none in voxel-core. No on-disk format change, no
  worldgen input, no bake. Config change in
  `ue-project/Config/DefaultEngine.ini` (`[/Script/Engine.UserInterfaceSettings]`),
  and a standing constraint on every widget in `ue-project/Source/VoxelEarthUI`.
- **Human sign-off:** Matt, 2026-09-07, verbatim: *"I'm directing you to adopt the
  scale tolerant approach for our UI."* Given after a walk-through of how
  production games handle resolution, and after he raised the symptom that
  started this: *"What's up with the UI menus looking bigger, bloated, blowup in
  size on my 1440p monitor during testing versus their initial html design files
  that were small and very fine detailed."*

## Context

**The symptom, and why it was not what it looked like.** The Voxelmark mocks are
authored at 1920x1080 and every `.html` in `docs/ui-mocks/2026-09-07/` carries its
own `zoom:1.33333` so it previews as 1440p. `VoxelUITheme.h` deliberately stores
the **1080p** figures and lets Slate supply the 1.333 (its Title-screen note says
so). `DefaultEngine.ini` had **no** `[/Script/Engine.UserInterfaceSettings]`
section, so the engine default applied — `ShortestSide` with keys
(480, 0.444) (720, 0.666) (1080, 1.0) (8640, 8.0), which is exactly
`shortestSide / 1080` at every point and therefore exactly **1.333** at a 1440
shortest side. Port and zoomed mock agreed to the pixel: a 1060x760 authored
shell rendered 1413x1013, 55% of a 2560 width.

So the **size** was never wrong. What the owner was comparing against was the
design canvas, which fits an artboard to its window and shows it well under 1:1.

**The real defect was crispness, and its cause is the art, not the policy.** A
fractional scale lands a 1 px border between two screen pixels and thickens it.
`menus_shared.css` sets `image-rendering:pixelated` and
`-webkit-font-smoothing:none`, and the design leans on 1 px rules, so the
fine detail it is made of degrades at any non-integer factor.

**What production does, and the two halves of it.** Design at one reference
resolution — 1920x1080, near-universally — express dimensions in reference units,
and let the engine multiply by a factor from the player's screen. Unreal's
`UIScaleCurve` keyed on shortest side and Unity's canvas scaler are the same
mechanism. 1080p is the reference because it is still roughly half of all players
(Steam hardware survey), because you author at the **lowest** resolution you
support so legibility failures are visible while you design (scaling up costs
sharpness, scaling down costs information), and because 1080 doubles exactly into
4K. The half that is easy to miss: production UI scales to *any* fractional factor
without artifacts because its primitives have no fixed pixel size — outline or
SDF text, nine-slice borders, vector or oversized icon art.

**Three options were put to the owner.** *Scale-tolerant*: fix the art so any
scale works, scale stays continuous. *Pixel-perfect*: integer scales only (the
arm briefly configured on 2026-09-07, since superseded — at 1440p it offers only
1.0 at 41% width or 2.0 at 83%, nothing near the designed 55%). *Hybrid*: keep the
art and constrain the scale.

**The evidence that decided it.** `menus_shared.css` declares `VT323` for body and
`Macondo Swash Caps` for display. Both are TrueType **outline** fonts with no
fixed pixel grid; they rasterise cleanly at any size. The mock's crispness comes
from switching off font smoothing, a rendering choice, not a size constraint. So
the text is already scale-tolerant, and the only genuinely fragile primitives are
the 1 px borders (the stylesheet already uses 2 px elsewhere) and any bitmap icon
art. Scale-tolerant is therefore **cheap** here, and the hybrid's permanent
two-rule complexity is unjustified.

## Decision

**1. One reference resolution: 1920x1080.** Every dimension in
`VoxelUITheme.h` and every widget is an authored 1080p figure. Never rescale a
constant to compensate for the display; the engine scale does that.

**2. Continuous scaling, `ShortestSide`, anchored 1.0 at 1080.** The curve is
written explicitly in `DefaultEngine.ini` rather than left to the engine default,
so an engine upgrade cannot silently move it. Its values reproduce
`shortestSide / 1080`. Shortest side is required, not incidental: it is what stops
an ultrawide monitor inflating everything.

**3. No pixel-locked primitives.** This is the binding constraint on new work:

- **Never a 1 px border.** Minimum 2 px, so it survives any scale factor.
- **Text uses the shipped outline faces** (`VT323`, `Macondo Swash Caps`,
  `IM Fell`, and the pixel face when supplied). No bitmap font may become
  load-bearing for layout.
- **Icon and ornament art is vector, drawn as Slate geometry, or authored at
  least 2x the largest size it will ever be shown at.** Never authored at exact
  target size and never relied upon to be sampled 1:1.
- **No dimension may depend on landing on a whole device pixel.** Fractional
  authored values are still discouraged (they are pointless), but nothing may
  *break* when the product of value and scale is fractional.

**4. Scale is not a substitute for layout.** Anchoring and responsive containers
handle aspect ratio; scale handles size. A panel that must not exceed a width
gets a max width, not a smaller scale.

**5. The player gets a manual override.** An `INTERFACE SIZE` row ships in
Settings (`VoxelGraphicsUserSettings`, `[VoxelGraphics] UIScale`, default 1.00,
0.75–1.50 in 0.05 steps, applied live through
`FSlateApplication::SetApplicationScale`), per the standing settings-panel policy
that every visual trade ships as a player-facing row. It multiplies the engine
scale; it does not replace it.

## Consequences

- On a 1440p screen the interface again occupies the proportion it was designed
  for (a 1060x760 shell renders 1413x1013, 55% of width). It will look **larger**
  than the pixel-perfect arm briefly configured earlier the same day. That is
  intended.
- Text is anti-aliased at whatever size the scale asks for, so hard edges are
  marginally softer than the browser's no-smoothing preview. This is the price
  paid, knowingly, for resolution independence.
- Every 1 px border in the port must be promoted to 2 px. This is a visual change
  of its own and should be judged on a capture, not assumed invisible.
- Any absolute pixel figure measured from a capture taken before 2026-09-07
  afternoon was measured at 1.333 and must be divided by it before comparison.
- Constants hand-tuned against a 1.333 capture are suspect and must be re-derived
  from the mock's own px value. Known at time of writing: `TitleBoxWidth`,
  `LogoRight`, `DlgDimAlpha`, and the owner-look items `TitleFontSize` and
  `PausePanelWidth`.
- **Re-authoring the mocks at 2560x1440 is explicitly rejected.** It would move
  the design above the legibility floor and make every 1080p player receive a
  downscale, which is the direction that loses information.
- Verification is one measurement, and it can fail: on a 2560-wide capture the
  in-game screen shell must measure ~1413 px. ~1060 px means a scale of 1.0 is in
  force and the curve did not take.

## Known violations at time of writing

**The six menu backdrops are the one real breach of decision 3, and it is art, not
code.** `ue-project/Content/UI/Backgrounds/*.jpg` are 1920x~1070 and drawn
full-bleed by `SVoxelCoverImage`, so on a 2560-wide screen they are **upscaled
1.33x**, and 2x at 4K. Photographic, so it reads as softness rather than
aliasing, which is why it went unfiled for months and why it contributed to the
"bloated" impression that opened this ADR.

A clarification the rule needs: **full-bleed photographic art wants >= 1x the
largest displayed size, not >= 2x.** Downsampling a photograph is safe and
upsampling is not, so the standard is "never upscaled". The >= 2x rule in
decision 3 is for icons and ornaments, where sampling near 1:1 causes visible
aliasing. A 2x-of-4K backdrop would be 7680 px and is not wanted.

**The fix is one command and is blocked only on the source art.**
`ue-project/Tools/prepare_ui_assets.py` already takes `--max-width`, and
`MENU_ART_CREDITS.md` records that the originals are 2754x1536 to 5524x3072 in
the Mira-Thal checkout at `assets/menu_backgrounds/`. That checkout
(`/home/user/Test` when the credits were written) **is not on this machine**, so
regeneration needs the owner to supply it. Then:

    python ue-project/Tools/prepare_ui_assets.py --max-width 3840 --max-height 2160 --source <menu_backgrounds>

3840 is chosen because it is exactly 1:1 at 4K and a clean downscale everywhere
below. Re-encoding is reproducible: the credits file pins each source by sha256,
so a swapped source shows in the diff rather than only in the pixels.

**Not violations, checked and recorded so nobody re-audits them:** the map
hillshade is 1950x2085 into a ~800x930 frame, i.e. downscaled ~2.2x; every rule,
ring, plate, well, track and ornament is Slate geometry over a 1x1 white brush;
there is no icon texture in `Content/UI` at all and `FVoxelItemDef` has no image
field; all four shipped faces are TrueType outlines.

Related: [[ADR-0009]] (Slate front end and committed UI art),
`docs/ui-mocks/2026-09-07/README.md`, `docs/front-end-plan.md`,
`VoxelUITheme.h` (the design tokens this ADR governs).

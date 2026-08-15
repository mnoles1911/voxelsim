# `refs/` — measured reference data for the generator

Everything in this directory is **checked in and read offline**. No build, no
probe and nothing under `forge/` fetches anything. `forge/language.py` is fully
local by owner instruction and this is held to the same standard.

Exactly one command touches a network, and it is never run by a build:

```
python tools/reffit.py fetch      # the ONLY networked step
```

`docs/reference-fitting-research.md` is the working: sources, licences,
ADOPT/REJECT with numbers, and what could not be sourced.

## What is here

| path | what it is | written by |
|---|---|---|
| `silhouettes/<species>/*.png` | licence-clean side silhouettes, one file per reference | `reffit.py fetch` |
| `silhouettes/SOURCES.json` | per FILE: uuid, licence, licence URL, contributor, source page, raster URL | `reffit.py fetch` |
| `silhouettes/ATTRIBUTION.md` | the attribution obligation of the CC BY files, discharged | `reffit.py fetch` |
| `species-latin.json` | spec name → scientific name, with `checked_by_hand` | `reffit.py fetch`, `refnames.py --write` |
| `excluded.json` | silhouettes refused BY HAND after looking, each with its reason | hand-edited |
| `quadruped-reference.json` | the extracted numbers — this is what `fit` and `report` read | `reffit.py extract` |
| `withers-target.json` | every species' authored shoulder height, frozen **once**, with the biome file's figure in metres beside it | `refstance.py freeze` |

`withers-target.json` is frozen rather than recomputed for one reason:
`quad.shoulder_h` is both the row being solved and the row that carried the
target, so a second run reading its own output would lower every animal again
and print a tidy table doing it. See `docs/quadruped-stance-height.md`, and
`tools/retune_quad_bulk.py`, which hit the same trap and fixed it the same way.

## The three rules that matter

**1. A licence is read off the file, never inferred from the site.** This
library has already refused two datasets on licence grounds — FishBase is
non-commercial, FishShapes contradicts itself between CC0 and CC BY-NC — so
`SOURCES.json` carries a `licence_url` per image, taken from PhyloPic's own
`_links.license.href`. `tools/reffit.py` accepts CC0, Public Domain Mark and
CC BY only, and refuses NonCommercial, ShareAlike and NoDerivatives **at
download**, so a refused file never reaches this directory. 36 were refused.

**2. A scientific name must be hand-checked before it can move a spec.** GBIF's
common-name search resolved `lion` to *Macaca silenus* and `nile-monitor` to
*Mungos mungo*, and **the mongoose reference passed every quality test this
pipeline has** — two silhouettes, both measurable, in close agreement. Nothing
downstream can tell a good measurement of the wrong animal from a good
measurement of the right one. So `tools/refnames.py` verifies each binomial
against GBIF's own vernacular list, and `reffit.py fit` skips any species whose
`checked_by_hand` is not `true` — printing which ones, rather than dropping them
quietly.

**2b. A verified NAME is not the same thing as verified PICTURES**, and the gap
between them was live in this directory until 2026-08-15. `species-latin.json`
records what a name was checked to be; `SOURCES.json` records what the files were
downloaded **as**. Fix a name by hand without re-fetching and they disagree, and
everything downstream reports a verified species measured off the wrong animal —
which is the mongoose again, one step further along. `common-frog` carried a
verified *Rana temporaria* over two silhouettes downloaded as *Mustela lutreola*,
a European mink, because the Mammalia constraint that fixes the shrubs cannot
return an amphibian. `reffit.py fit` now refuses any species where the two names
differ and prints the `fetch` command that repairs it.

**3. What the automatic gate cannot catch is written down, not papered over.**
`excluded.json` holds silhouettes that pass every geometric test and are still
wrong — mostly sitting animals, whose haunch measures exactly like a very thick
leg. Four discriminators were built and measured before accepting that this
cannot be automated; the numbers are in `excluded.json` itself and in
`docs/reference-fitting-research.md` §3.3. The remaining defence is
`MAX_LIFT = 1.5` in `tools/reffit.py`, which bounds what any undetected bad
reference can do to a spec.

## Re-running it

```
python tools/refnames.py --write     # verify names first; fit depends on it
python tools/reffit.py fetch         # ONLINE
python tools/reffit.py extract       # silhouettes -> quadruped-reference.json
python tools/reffit.py report        # how far the library is from life
python tools/reffit.py overlay       # LOOK at where each measurement landed
python tools/reffit.py fit --dry     # what would move
```

A species whose scientific name changes has its old silhouettes deleted on the
next `fetch` — files only, one directory deep, never a recursive tree delete.
This machine has lost a cache to a recursive delete that followed a Windows
junction and `refs/` is not worth repeating that on.

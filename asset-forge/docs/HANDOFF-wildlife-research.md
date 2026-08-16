# Handoff: finish the wildlife density research

**For a fresh session with a working web-search budget.** Everything here is
research — sourcing numbers for species that currently have none. No engine work,
no editor, no Unreal. `asset-forge` and its `tools/` only.

Written 2026-08-16 at the end of a session that exhausted its 200 web searches
before reaching this work.

---

## 0. Read these three first, in this order

1. `docs/wildlife-density-research.md` — what was sourced, how, and the two
   things it got wrong on the way.
2. `tools/densityref.py` — the tool. Its docstring is the method.
3. `tools/refnames.py` — **read the top of this before touching a single name.**
   It records what happened when names were bulk-resolved: GBIF returned a
   lion-tailed macaque for `lion`, a banded mongoose for `nile-monitor`, and an
   extinct marsupial for `fisher` — each a single confident result with a clean
   binomial that then produced a valid, internally agreeing, **completely wrong**
   reference. This is the single most important constraint on the job below.

## 1. Where it stands

`refs/density-reference.json`, 149 species with names on file:

| tier | n | meaning |
|---|---|---|
| measured | 84 | PanTHERIA species-level figure |
| allometric | 26 | predicted from body mass **and** trophic level |
| genus | 10 | median of congeners |
| **none** | **29** | no source. Not estimated. |

Names: **137 verified** (all 131 quadrupeds + 18 cetaceans), 0 rejected.

**The source in hand:** PanTHERIA (Jones et al. 2009, *Ecology* 90:2648), checked
in at `refs/raw/PanTHERIA_WR05.txt` with its licence in
`refs/raw/SOURCES.json`. Mammals only.

## 2. The work, in priority order

### 2a. Birds — 127 species, nothing at all

Neither names nor densities. **Names first**, hand-supplied and GBIF-verified
through `refnames.py`'s existing check (ACCEPTED, SPECIES rank, expected class,
GBIF's own vernaculars overlapping the spec name). Add them to that file's `HAND`
table; do not write a bulk resolver.

Then densities. Birds are usually reported as **breeding territories or pairs per
km²**, which is *not* individuals per km² — a territory is a pair plus that
year's young, so a conversion is needed and it must be recorded per source, not
applied as a blanket ×2. Candidate directions, none verified because the search
budget ran out:

* North American Breeding Bird Survey (USGS) — long-running, open, route-based
  relative abundance; converting routes to per-km² is the hard part.
* European Bird Census Council / PECBMS — territory densities per habitat.
* **AVONET** (Tobias et al. 2022, *Ecology Letters*) — open, ~11,000 species, and
  it carries body mass and habitat but **not density**. Still worth vendoring: it
  gives the mass term the allometric fallback needs.

**Fit a bird-specific allometry.** Do not reuse the mammal Damuth fit. Refit the
same way `densityref.fit_allometry` does — on bird data, split by trophic level —
and report the slope and residual scatter so the estimate carries its own error
bar. The mammal fit came out at −0.741 against Damuth's −0.75, which is the
evidence that made the fallback trustworthy; earn the same evidence for birds.

### 2b. Fish — 106 species, nothing at all

Hardest of the three, for a reason worth understanding before starting: fish are
reported as **standing biomass per hectare** (kg/ha), not counts. Converting to
individuals needs a mean body mass per species, and the result is extremely
habitat-dependent — a trout stream and a carp pond differ by more than the
species do.

* FishBase has the traits, but a previous session **rejected it on licence** —
  re-check the terms before vendoring anything, and record the finding either way.
* Note the library already distinguishes salt/fresh/river/lake/reef
  (`docs/asset-placement-architecture.md` §6), so densities can be authored per
  water class rather than per species-global.

**It is legitimate to conclude that per-species fish density is not sourceable
and that a per-water-class default is the honest answer.** Say so with the
evidence if that is where it lands.

### 2c. Cetaceans — 18 species, names done, densities refused

Names are complete and verified. **Densities were deliberately refused**, and the
reasoning must not be undone casually: PanTHERIA carries only five marine mammals
with both mass and density, spanning 1,200× with **no relationship to body mass**
— a 27-tonne grey whale at 4.90/km² against a 50 kg porpoise at 0.03. Those are
local survey densities in feeding aggregations, not range-wide. Same units,
different quantity.

`densityref.py` has a guard, `NO_TERRESTRIAL_ALLOMETRY = {Cetacea, Sirenia}`,
which exists because the tool handed fourteen whales terrestrial *carnivore*
predictions minutes after that was shown to be invalid. **Do not remove the guard
to improve coverage.** Replace it with a real marine source or leave it.

Direction: IWC and NOAA stock assessments give abundance per stock over a survey
area. That is convertible, but the survey area must travel with the number.

### 2d. Reptiles and amphibians — 13 species

`common-frog`, `fire-salamander`, `poison-dart-frog`, `marine-iguana`,
`nile-crocodile`, `spectacled-caiman`, and seven lizards/monitors. PanTHERIA does
not cover them at all. Herpetological densities exist in the literature but are
scattered, plot-based and wildly habitat-dependent.

**Ask the owner whether these are worth chasing** before spending a budget on
them — an allometric estimate flagged as such may be entirely good enough for a
sand lizard. The owner has not been asked.

### 2e. `arctic-ground-squirrel` — one species

Present in PanTHERIA under *Spermophilus parryii* (the WR05 synonym, already
bridged) but with **no density recorded**. Needs a direct source or the genus
fallback.

## 3. Rules this work must hold to

1. **`fetch` is the only command that may touch a network.** Everything `extract`
   needs is checked in. This mirrors `reffit.py` and it is not negotiable —
   `forge/language.py` is fully local by owner instruction and the reference
   tooling is held to the same standard.
2. **Nothing under `forge/` may import a reference tool.** Check with a grep
   before finishing.
3. **Every number carries its tier and its source.** A figure with no provenance
   is the thing this project keeps getting hurt by. `none` is a valid, honest
   answer and is preferred to a plausible invention.
4. **Vendored data gets a `SOURCES.json` entry** with citation, URL, terms and
   the columns actually used — see `refs/raw/SOURCES.json`.
5. **Names are hand-supplied and verified, never bulk-resolved.** See §0.3.
6. **Re-run `python tools/densityref.py report`** and paste the tier counts into
   the commit message.

## 4. What "done" looks like

* Every one of the 382 animal species either carries a density with a named tier,
  or appears in the `report`'s NO SOURCE list with a reason.
* Bird and (if sourceable) fish allometries fitted on their own data, with slope
  and residual scatter reported, not borrowed from mammals.
* `docs/wildlife-density-research.md` extended with what was found, what was
  refused, and what could not be sourced.
* Gates: `python -m forge.cli selftest` PASS. Nothing here should touch a spec,
  so `buildcheck` should be unnecessary — if a spec moved, something went wrong.

## 5. What this research feeds, so the numbers are used correctly

* `docs/wildlife-lod-and-rings.md` — density decides *composition*, body size
  decides *existence*. These numbers set what is rare, not how many entities run.
* `docs/wildlife-behaviour-decisions.md` — the trap model consumes density, home
  range and activity cycle directly. A 20-snare line on real densities already
  yields a rabbit every 0.7 days and a fox every 24, **with no inflation**, so
  changing a density changes gameplay immediately.
* The owner's standing decision is **real densities for common species, boosted
  multipliers for rare ones**, with the multiplier authored separately and never
  folded into the measured figure. Keep the baseline honest; the game tuning
  lives elsewhere.

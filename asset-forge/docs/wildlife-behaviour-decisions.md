# Wildlife behaviour: the owner's eight decisions, and what each one costs

Taken 2026-08-16, in answer to questions put with their trade-offs. This file is
the record of what was decided and what it now obliges the build to do. Where the
owner chose against my recommendation it says so, because the reasoning that lost
is worth keeping.

---

## The decisions

| # | Question | Decision |
|---|---|---|
| 1 | How does a trap catch anything while the player is away? | **Roll it when you return** |
| 2 | What happens to a killed animal long-term? | **Comes back after some in-game days** |
| 3 | How real should densities be? | **Real for common, boosted for rare** |
| 4 | What are the 546 small colony animals inside bow range? | **Cheap until you aim at one** |
| 5 | Do predators come after the player? | **Yes, predators hunt you** *(against my recommendation)* |
| 6 | Can an animal avoid a badly placed trap? | **No — bait and luck only** *(simpler than I proposed)* |
| 7 | A catch left in a snare for a week? | **It rots or gets taken** |
| 8 | How wary are animals? | **Stealth system, fairly simple, based on NOISE** |

---

## 1. Traps resolve on return, and the density data is the input

Nothing is simulated while the player is away. A trap is a persistent record —
position, type, bait, time set — and on the player's return the catch is rolled
from elapsed time and the local eligible species.

**This is the second payoff from `docs/wildlife-density-research.md`.** The roll
is not a made-up number: it is the species' measured density, its home-range
(how much ground it sweeps in a day) and its activity cycle, all of which came
free in the same PanTHERIA download. Coverage of our 131 quadrupeds: density
113, home range 80, trophic level 97, activity cycle 90.

The physical model is

    catches per day = density(/km²) × daily path(km) × capture width(km)

with daily path taken as `2 × √(home range)` — an order-of-magnitude stand-in,
flagged as such — and capture width ~50 cm for a snare.

### The calibration, and it works without inflation

One snare catches a European rabbit every 14 days, which sounds unplayable. It
is the wrong unit: **real trapping is a line, not a snare.** At 20 snares, on
real uninflated densities:

| species | one every |
|---|---|
| european-rabbit | **0.7 days** |
| european-hare | 8.2 days |
| roe-deer | 9.1 days |
| red-fox | 24.3 days |

Something most mornings, small game as the staple, a deer in a snare as a
genuine event and a fox as a prize. That is a working trapping game **produced by
the real numbers**, and it rewards investment in a line, which is what actual
trapping rewards. No inflation is required here — decision 3's boost is for
encounter rates, not for traps.

### What must still be true

The rate a player *observes* when watching a trap must match the rate rolled when
they are away, or they will notice — "traps only work when I'm not looking" is a
bug players find within an hour. One model, used by both paths.

## 2. Kills persist as a ledger, not as animals

A kill writes a small permanent record; that individual stays dead for a set
number of in-game days. Storage is proportional to what the player has done, not
to the size of the world — the same shape the terrain overlay already uses, where
only edits are stored and they win over the generated world.

This requires stable identity, which the scatter already provides: site id plus
index within the herd, plus a respawn epoch. **The number of days is not chosen
yet** and is a feel decision, best made against a playable build.

## 3. Real for common, boosted for rare

Deer, rabbits and birds keep their measured densities. Predators and spectacle
animals get a per-species multiplier. The baseline is
`refs/density-reference.json` so that "we doubled the wolves" is a sentence with
a meaning; the multipliers belong in the spec, next to the density, never folded
into it.

## 4. Small animals are cheap until targeted

The 546 prairie dogs, moles, rabbits and marmots inside bow range move, flee and
look alive with no real AI. Targeting one promotes that single animal to a full
actor. This is the promotion-on-demand mechanism that was invented for long shots
in `wildlife-lod-and-rings.md` §4 and then made unnecessary by the 200 m weapon
ceiling — it survives here, for a much cheaper reason.

## 5. Predators hunt the player — and this is the risky one

**I recommended against this and the owner chose it, which is his call.** The
reason to record the objection rather than bury it: decision 3 boosts predator
densities *and* decision 5 makes predators aggressive, and those two multiply. A
wolf at its true 0.01/km² that hunts you is a memorable event; a wolf boosted 50×
that hunts you is a world where travel is not possible.

The mitigation is not to revisit the decision, it is to make the interaction
visible while tuning: **predator boost and predator aggression must be tuned
together, against a number** — encounters per hour of travel — rather than
separately by feel. That number does not exist yet and should exist before the
first boost multiplier is authored.

Combat AI needed: approach, circle, commit, retreat, break off when wounded.
`herd.flee_m` (default 60 m) already exists and is the wrong side of this — it
describes prey fleeing, not predators closing.

## 6. Traps are bait and luck, with no placement skill

Every trap has the same odds, modified by bait matched against the species'
trophic level (plant bait for herbivores, meat for carnivores — a column we
already have for 97 of 131 species). No hidden "how well placed is this" score.

Simpler than what I proposed, and it has a real virtue I underrated: it is
**explainable**. A player can be told exactly how trapping works and can reason
about it, whereas a placement-quality score is a number they can only guess at.
The cost is that trapping does not reward learning the land, so the depth has to
come from *where* and *how many*, not from craft.

## 7. A catch rots or is taken

An uncollected catch spoils or is claimed by a scavenger, and the player finds
remains. This gives a trap line a schedule, which is what a trap line is, and it
stops traps becoming an infinite meat bank.

It falls out of decision 1 almost for free: the roll already knows the elapsed
time, so it can produce "caught, then spoiled" as easily as "caught". Finding
remains rather than nothing also solves decision 6's information problem — the
player learns their placement worked even when they lost the meat.

## 8. Stealth on noise alone

Animals flee at distance; the player closes by being quiet. **Noise only — no
scent, no wind direction.** That is a deliberate simplification of what I
proposed and it removes the largest single AI job on the list.

What it needs: a noise value per player action (crouched, walking, running, on
what surface), a hearing radius per species, and the existing per-species flee
distance. What it explicitly does not need: a wind field, a scent plume, or
anything that makes the player think about which way they approach from.

Worth flagging honestly: with noise as the only channel, a player who crouches
becomes invisible regardless of position, so the counter-play is distance and
patience rather than terrain. If stalking later feels flat, wind is the first
thing to add and the design should leave room for it.

---

## What is still open

* **The respawn interval** (decision 2) — days, needs a playable build.
* **The predator boost multipliers** (decisions 3 + 5) — and the encounters-per-hour
  measurement they should be tuned against, which does not exist yet.
* **Bird, fish and cetacean densities** — 251 species with no source at all.
  PanTHERIA is mammals only, and those species have no scientific names on file
  yet. Blocked on research budget, not on decisions.
* **Whether traps can catch birds and fish** — nets and fish traps are the same
  mechanism with different data, and that data is the item above.

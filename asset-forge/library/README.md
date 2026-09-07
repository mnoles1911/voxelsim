# `library/` — kept individuals, and the category index

An asset here is never a blob of voxels we have to keep. A species is
`(spec, seed)`, a few hundred bytes that regenerate byte-for-byte the same
voxels every time. This directory holds the ones somebody chose to keep built,
so the library can be browsed — and, for the kinds that produce no bank, so the
engine has a file to load.

    library/<species>/<species>-<seed>/
        spec.json       the species, as authored and validated
        realized.json   this individual of it
        tree.vxa        THE GRID THE ENGINE LOADS  (v3; carries its own voxel_mm)
        tree.vox        MagicaVoxel / Blender round trip
        thumb.png       the kind's own review camera
        meta.json       stats, health problems, spec_hash, category

Written by `forge.server.keep` — the same function the app's **Keep to library**
button calls, so there is one writer and one layout.

## `categories.json` — the query seam

    python tools/export_categories.py            # write it
    python tools/export_categories.py --check    # fail if it is stale

One generated file answering one question: **what is each asset, and where is
its grid.** It is the seam a crafting system reads to enumerate the items a
player can make:

    categories.craftable.species   ->  [{name, kind, voxel_mm, via, curation,
                                         grids: [{seed, vxa, voxels, ...}]}]

Every row goes through `forge.categories.of`, which is also what the app and
`tools/buildcheck.py --category` read, so a crafting system and the forge cannot
disagree about what is craftable. Paths are relative to the asset-forge root and
are emitted only when the file is **on disk** — an index naming a path nothing
wrote fails at load time in the game, which is much later and much worse than
failing here.

**Why this is not in the VXM manifest.** `species.vxm` is worldgen input: it
carries per-kind × per-biome densities for the per-chunk scatter, and its
`KIND_ORDER` mirrors `assetmanifest.h`, an append-only **engine** contract. A
craftable has no density, no biome and no scatter — it is refused from that
manifest by name (`manifest.KINDS_ENTITY`), deliberately, per ADR-0010. Putting
a crafting index there would mean appending a kind to an engine contract in
order to describe something world composition must never see.

**Refusals are listed, not dropped.** A species whose `category` block is
illegible resolves to nothing and appears in `refused` with the reason. A
species that vanished from an index would be invisible; one in a refusal list
has somebody to tell.

## What is *not* here

Banks. `banks/<species>/<species>-<seed>.vxa` is the shipping bake for
world-composed content and is written by `tools/export_banks.py` (terrain kinds
by default). Craftables and creatures produce no bank — a craftable's shipping
grid is the library individual above, which is why `categories.json` points at
`library/` and not at `banks/`.

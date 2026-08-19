# Asset Forge web UI

The React frontend for asset-forge: the species library, curation, placement
(biomes, allowlists, named rule overrides), rules authoring, and import of
outside voxel assets. Medieval, voxel-stylized: dark stone and parchment,
chamfered corners and hard bevels instead of rounded glass, lucide icons only.

## Stack

- Vite + React 19 + TypeScript (strict)
- Tailwind CSS 4 (theme tokens in `src/index.css`)
- Radix UI primitives (select, checkbox, dialog), restyled shadcn-fashion in
  `src/components/ui/`
- lucide-react for every icon -- no emoji, no raw-text glyphs anywhere
- The 3D variant viewer is a TypeScript port of `forge/web/viewer.js`
  (`src/lib/viewer.ts`): one instanced WebGL2 draw call of unit cubes over the
  server's binary surface-voxel blob.

## Running it

Normal use is ONE process -- the forge server serves the built app:

    cd asset-forge
    python -m forge.cli serve          # serves web/dist/ at http://127.0.0.1:8731

`forge/server.py` serves `web/dist/` whenever it exists and falls back to the
legacy page in `forge/web/` only on a checkout that never ran a build. To
rebuild after changing the frontend:

    cd asset-forge/web
    npm install
    npm run build                      # tsc --noEmit && vite build -> dist/

For iteration, a dev server with hot reload proxies every `/api` call to the
running forge server:

    python -m forge.cli serve          # terminal 1, port 8731
    cd asset-forge/web && npm run dev  # terminal 2, http://localhost:5173

## The screens

**Library** (home). The whole species ledger -- all specs in `specs/` --
filterable by kind, biome (including "unassigned"), curation verdict and text
search. Selecting a species opens its panel:

- *Kept variants*: every library entry for the species, generated or imported,
  with thumbnails, the orbiting 3D viewer (drag orbits, wheel/pinch zooms,
  double-click resets), downloads (.vox/.vxa/spec) and delete.
- *Publish verdict*: approve / reject / back-to-draft plus the per-seed
  toggles and notes. Writes `POST /api/curation`, which touches only the
  curation block of the raw spec file. "Unreviewed" species export under the
  grandfather clause and say so.
- *Placement*: the owner's flow in order. (1) Biome membership by checkbox
  with a 0..1 weight each. (2) Allowlist semantics by dropdown: derived from
  the weights / explicit list (per-biome checkboxes appear) / allowed nowhere.
  (3) Named rule overrides attached per biome from a dropdown of the rule
  library, shown as removable chips; several rules on one biome show their
  composed (intersection, strictest-wins) effect and flag genuine conflicts.
  The kind x biome density column is read-only context. Saving writes
  `POST /api/placement` -- only the edited blocks of the raw spec move.

**Placement rules**. The named-rule library itself (`rules/placement-rules.json`).
Rules are authored here FIRST, then attached by name in the library. Create /
edit / delete, with every range validated client-side against
`src/lib/schema.ts` before any write (the server re-checks). Deleting a rule
any spec still cites refuses, naming the species -- a dangling reference would
make the export refuse later, so it cannot be written at all. The kind x biome
density table renders read-only at the bottom.

**Import asset**. Accepts MagicaVoxel `.vox` (palette colours snap to the
nearest forge material) or the forge's own `.vxa`, with a species name, kind
and (for .vox) voxel size. The server writes the library entry AND a spec file
starting at curation `draft`, so an imported asset is curated and
placement-specced through exactly the same panels as a generated one; nothing
in the UI assumes generator-only fields.

## Where the schema lives

`src/lib/schema.ts` is the single seam to the placement-spec contract
(`docs/placement-spec-schema.md`; server-side: `forge/spec.py`,
`forge/biomes.py`, `rules/*.json`). Field law, clamps, composition semantics,
allowlist modes and every shared type live there; adapting to a schema change
is that one file plus its mirror clamps in `forge/server.py` (`RULE_CLAMPS`).
Biome keys, kinds, rule names and the density table are always fetched from
the server, never hardcoded.

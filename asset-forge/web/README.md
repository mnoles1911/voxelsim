# Asset Forge web UI

The React frontend for asset-forge. It follows the owner's four-stage
workflow end to end: generate a 3D asset within a class, fine-tune it against
the live preview, keep it to the library, then set its placement (biomes,
allowlist, named rule overrides) on the asset's spec. Medieval, voxel-stylized:
dark stone and parchment, chamfered corners and hard bevels instead of rounded
glass, lucide icons only.

## Stack

- Vite + React 19 + TypeScript (strict)
- Tailwind CSS 4 (theme tokens in `src/index.css`)
- Radix UI primitives (select, checkbox, dialog), restyled shadcn-fashion in
  `src/components/ui/`
- lucide-react for every icon -- no emoji, no raw-text glyphs anywhere
- The 3D viewer is a TypeScript port of `forge/web/viewer.js`
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

## The workflow, screen by screen

**Stage 1-2 -- Forge tab** (the home). Pick a kind and a species; the
parameter rail is GENERATED from `/api/schema` (forge/spec.py's parameter
rows), grouped as the schema groups them and clamped to its ranges, so a new
kind's controls appear with zero UI work. Generate forges a block of seeds
(`/api/generate`), the progress bar polls the job (`/api/job`), and tiles fill
in as the server renders them. Fine-tune by dragging sliders -- "auto-forge on
release" regenerates on every commit, changed parameters highlight gold until
the spec is saved, Revert restores the file's values. Reroll jumps to a random
seed block; More seeds extends with the next block. The "Plain speech" box is
`/api/interpret` (forge/language.py): a fully LOCAL vocabulary -- no model
callbacks, nothing leaves the machine -- that turns "taller, sparser crown"
into parameter edits and says out loud which words it did not understand.
Clicking a tile opens the seed in the orbiting 3D viewer with its build
statistics and health flags. `Save spec` writes the parameters to
`specs/<name>.json` (rename first to fork a new species).

**Stage 3 -- Keep to library.** Every tile carries `keep` (save this variant:
`/api/keep`) and `bank` (toggle the seed in the PUBLISHED bank -- the curation
seeds, a separate decision from keeping a portfolio entry). "Keep all clean"
saves every unflagged tile; flagged ones are skipped on purpose. Once kept,
the seed dialog offers **Set placement**, which jumps straight to stage 4 with
the species already open. The workflow strip across the top of the Forge shows
all four stages; stage 4 is a live link whenever the species has a spec file.

**Stage 4 -- Library & placement tab.** The whole species ledger, filterable
by kind, biome (including "unassigned"), curation verdict and text search.
The selected species shows:

- *Kept variants*: thumbnails, the 3D viewer, downloads (.vox/.vxa/spec),
  delete, and "More like this" -- which reopens the entry's exact spec in the
  Forge at a fresh seed block (the loop back to stage 1).
- *Publish verdict*: approve / reject / draft, per-seed toggles, notes.
  Writes `/api/curation`; only the curation block of the raw spec file moves.
  "Unreviewed" species export under the grandfather clause and say so.
- *Placement*: biome membership by checkbox with a 0..1 weight; allowlist
  semantics by dropdown (derived from weights / explicit list / nowhere);
  named rule overrides attached per biome from the rule library, with a
  composed-intersection preview and conflict flags; the kind x biome density
  as read-only context. Saving writes `/api/placement` -- only the edited
  blocks of the raw spec move.

**Placement rules tab.** The named-rule library itself
(`rules/placement-rules.json`). Rules are authored here FIRST, then attached
by name in the library. Create / edit / delete, ranges validated client-side
against `src/lib/schema.ts` before any write (the server re-checks). Deleting
a rule any spec still cites refuses, naming the species. The density table
renders read-only at the bottom.

**Import asset** (header button). Accepts MagicaVoxel `.vox` (palette snapped
to the nearest forge material) or the forge's own `.vxa`. The server writes
the library entry AND a spec file starting at curation `draft`, then the app
opens the new species' placement panel -- an imported asset walks stages 3-4
exactly like a generated one, and nothing in the UI assumes generator-only
fields.

## Where the schema lives

`src/lib/schema.ts` is the single seam to the placement-spec contract
(`docs/placement-spec-schema.md`; server-side: `forge/spec.py`,
`forge/biomes.py`, `rules/*.json`). Field law, clamps, composition semantics,
allowlist modes, the UI-schema row shape and every shared type live there;
adapting to a schema change is that one file plus its mirror clamps in
`forge/server.py` (`RULE_CLAMPS`). Biome keys, kinds, rule names, parameter
definitions and the density table are always fetched from the server, never
hardcoded.

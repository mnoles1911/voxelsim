# Asset Forge as a daily tool: formalization plan

Written 2026-09-05. Read-only survey + plan; nothing here is implemented yet.

The owner's vision, verbatim:

> "My vision for the application is an executable/shortcut I can [run] from my
> desktop to open Asset Forge and once within Asset Forge I can 1) execute the
> procedural generation of 3D models given text/natural language inputs OR
> knobs and setting sliders, 2) select 'winning' assets to be saved into a
> library of approved assets that can then be used in my voxel sim game."

Every claim below is anchored. Where a phase has a gate, the gate is a counter
that can fail, per the house doctrine; the hash-isolation red arms are named
wherever a phase touches `spec.PARAMS` or the taxonomy.

---

## 1. Current state, verified

### 1.1 How the app starts today

- One command: `python -m forge.cli serve` (`asset-forge/forge/cli.py:335-339`,
  argparse wiring `cli.py:834-838`). `serve()` binds loopback:8731, prints the
  URL, and auto-opens the default browser after 0.6 s
  (`asset-forge/forge/server.py:1374-1399`). Loopback-only is deliberate — the
  app writes files and has no auth (`server.py:1384-1388`); `--host 0.0.0.0`
  opts into LAN + a printed phone URL.
- The server is stdlib `http.server`, zero web-framework deps (`server.py:1-14`).
  Python deps are `numpy`, `scipy`, `pillow` (`asset-forge/README.md:253`);
  there is **no** `requirements.txt` / `pyproject.toml` anywhere under
  asset-forge (verified by listing). System Python 3.12.10 runs the CLI clean
  (verified: `python -m forge.cli --help`).
- It serves the built React app from `web/dist` when present, else the legacy
  hand-written page (`server.py:42-43`, `server.py:1322-1331`; takeover note
  `server.py:38-41`). SPA fallback at `server.py:977-980`.
- **No launcher exists.** Grep across the repo for `forge.cli serve` / `8731`
  in `.ps1/.cmd/.bat` finds nothing but docs and CI (`.github/workflows/ci.yml:286`
  runs `selftest --quick`). There is no shortcut, no icon, no port-conflict or
  single-instance handling, and no shutdown story beyond ctrl-c in the console
  (`server.py:1394-1397`).
- **Hygiene flag:** `web/dist` is committed on purpose (web/.gitignore ignores
  only node_modules), and the on-disk build is fresh (built 2026-09-05
  12:14:36, after every source edit) — but git shows the committed bundles
  deleted, new hashed bundles untracked, and 7 modified `web/src` files whose
  last commit is 2026-08-19. ~17 days of frontend work is running from an
  uncommitted build.

### 1.2 Generation: knobs and sliders

- `pipeline.build(spec, seed, ...)` (`asset-forge/forge/pipeline.py:365`) is
  deterministic from `(spec, seed)`; the server never caches voxels, only PNG
  tiles memoised by `(spec_hash, seed, scale, preview_cm, camera)`
  (`server.py:121-229`).
- The whole slider surface is server-defined: `spec.PARAMS`
  (`asset-forge/forge/spec.py:214`) → `ui_schema(kind)` (`spec.py:3783`) →
  `GET /api/schema?kind=` (`server.py:704-714`). The frontend hardcodes **no**
  parameter names or ranges — `UiParam {path, kind, lo, hi, step, choices,
  group}` (`web/src/lib/schema.ts:291-308`) drives generated
  `<input type="range">` controls (`web/src/components/ForgeView.tsx:591-674`),
  committing on pointer-up with optional auto-regenerate
  (`ForgeView.tsx:209-211`).
- Batch flow: `POST /api/generate {spec, seed_start, count(≤200)}` starts a
  job (`server.py:993-1001`), the client polls `/api/job` at 350 ms
  (`ForgeView.tsx:127-135`). Buttons: Generate / Reroll / More seeds / Keep all
  clean (`ForgeView.tsx:395-441`). Detail view is a hand-written WebGL2 orbit
  viewer fed binary surface voxels (`server.py:904-941`,
  `web/src/lib/viewer.ts:162`).
- 11 kinds across 3 categories, 830 species; every kind registered in
  `asset-forge/forge/kinds.py:80-192`, category taxonomy in
  `asset-forge/forge/categories.py:94-124` with import-time asserts that a
  kind cannot arrive unclassified (`categories.py:135-149`).

### 1.3 Natural language: exists for EDITING, not for CREATING

- There is a first-class "Plain speech" panel in the Forge tab
  (`ForgeView.tsx:678-750`): text → `POST /api/interpret {spec, request}`
  (`server.py:1008-1015`) → `language.interpret`
  (`asset-forge/forge/language.py:1522`), a **local, deterministic** ~200-phrase
  vocabulary mapping onto parameter recipes (`README.md:196-241`). Unknown
  words are reported, not swallowed; edits land through `spec.patch` so they
  are clamped, visible as moved sliders, and revertable.
- The UI prints "N local concepts · nothing leaves this machine"
  (`ForgeView.tsx:717-721`), and comments state the no-network property as a
  standing constraint (`web/src/lib/api.ts:114-115`, `schema.ts:342-344`).
- What does **not** exist: creating a species from a sentence ("a gnarled dead
  willow for tundra") — kind and base species must be picked from selects
  first, and the vocabulary only patches the loaded spec. No LLM is involved
  anywhere.

### 1.4 The 'winning asset' flow as it stands

- **Keep** (per individual): `POST /api/keep` → `forge.server.keep`
  (`server.py:322-353`, route `server.py:1059-1062`) writes
  `library/<species>/<species>-<seed>/{spec,realized,meta}.json + tree.vxa +
  tree.vox + thumb.png` — the one writer, one layout
  (`asset-forge/library/README.md:9-18`).
- **Curation** (per species): `POST /api/curation` writes a
  `{status, seeds, notes}` block into the RAW spec file — nowhere else, so the
  diff is the verdict (`server.py:1025-1057`). Statuses are
  `draft/approved/rejected` (`spec.py:3166`); an **absent** block is
  grandfathered `approved` at seeds 1-4 with `curated: false`
  (`spec.py:3170-3196`). Measured today: **828 specs grandfathered, 2 draft**
  (`canoe`, `glider` — draft pending in-engine capture; verified by counting
  `curation` blocks across `specs/*.json`).
- UI: `SpeciesPanel`'s CurationBar has Approve / Reject / Back-to-draft, bank
  seed toggles, and verdict notes (`web/src/components/SpeciesPanel.tsx:128-210`);
  LibraryView filters by kind/category/biome/status, defaulting to `approved`
  (the exporting set) with an explicit "Never reviewed" filter
  (`web/src/components/LibraryView.tsx:34, 144-153`); per-variant Inspect /
  downloads / delete in `LibraryInspector.tsx:189-233`.
- **The gate is honoured at export, and only there.**
  `manifest.curated_inputs` is the single shared gate for the manifest writer
  and the staleness checker (`asset-forge/forge/manifest.py:723-772`);
  `tools/export_banks.py` honours it and **deletes** bank files for
  rejected/draft species, precisely because the engine scans the banks
  directory by layout (`export_banks.py:33-44`).

### 1.5 How the game consumes assets — two paths, one of them ungated

- **Path A — world/scatter content** (environment kinds):
  `tools/export_manifest.py:38` writes `out/engine/species.vxm`;
  `tools/export_banks.py` writes `out/engine/banks/<name>/<name>-NNNN.vxa`.
  UE reads both only when launched with `-VoxelAssetDir=<dir>`
  (`ue-project/Source/VoxelEarth/VoxelWorldSubsystem.cpp:6497-6531`), through
  `vxc::AssetBankLibrary` (`voxel-core/src/assetbank.cpp:86-149`), which loads
  **every** `.vxa` in a species directory unconditionally
  (`assetbank.cpp:106-115`). Curation is enforced **by omission from the
  export directory** — the engine never sees a status byte
  (`species.vxm` carries none: `voxel-core/include/voxelcore/assetmanifest.h:167-193`;
  zero `curation` hits in ue-project/voxel-core).
- **Path B — entity/craft content** (craftables): `UVoxelAssetBodyComponent::
  ResolveVxaPath` (`ue-project/Source/VoxelEarth/VoxelAssetBody.cpp:130-213`)
  probes `<root>/<name>/<name>-NNNN/tree.vxa` by name, where root defaults to
  **`<ProjectDir>/../asset-forge/library` — the authoring library, directly,
  uncooked** (`VoxelAssetBody.cpp:102-111`; cvar `voxel.AssetBody.LibraryRoot`
  `:27-35`). Consumers hardcode names: `voxel.Boat.Asset` = `"canoe"`
  (`VoxelBoat.cpp:35`), `voxel.Glider.Asset` = `"glider"` (`VoxelGlider.cpp:26`).
  **No index, no curation gate — the game is serving `draft` craft today.**
- **The query seam is authored and consumed by nobody.**
  `library/categories.json` (writer `tools/export_categories.py`, `--check`
  PASS at 830 species, wired into selftest `cli.py:739-753`) and its live twin
  `GET /api/categories` (`server.py:750-762`) both exist so a crafting system
  and the forge cannot disagree about what is craftable
  (`library/README.md:20-37`) — but the web frontend defines the call and
  never makes it (`web/src/lib/api.ts:26`), and grep finds no game-side reader.
- **ADR-0010** (`docs/adr/0010-two-lattice-jurisdictions.md`, accepted
  2026-08-27): the world ladder is exactly 100/50/25 mm; entities (creatures,
  craft) are the second jurisdiction — per-asset `voxel_mm` under a per-entity
  transform, no shared grid, never composed into the world.
  `manifest.species_record` refuses entity kinds by name
  (`manifest.py:929-938`); `export_banks.py:70-94` enforces the ladder at
  export and deliberately does not police entities.
- `.vxa` v3 (`asset-forge/forge/vxa.py:1-96`) carries `voxel_mm`, part runs and
  joints; v1/v2 are refused with named fixes (`vxa.py:170-187`). C++ read side
  is `voxel-core/include/voxelcore/assetgrid.h:85-170`.

### 1.6 The hash-isolation discipline (load-bearing; every phase must preserve it)

- Identity is `spec_hash` (`spec.py:3477-3503`) and the individual is
  `seed_hash` (`spec.py:3583-3606`). `notes`, `curation`, `biome_allow`,
  `biome_rules` and `category` are **excluded from both hashes**
  (`spec.py:3458-3474`) — a verdict or classification must not redraw the
  thing it describes (`categories.py:43-50`). `KIND_SCOPED_PARAMS`
  (`spec.py:3415`) plus the empty-container prune (`spec.py:3420-3455`) is
  what lets a new kind's parameters land without re-identifying all 830
  species.
- The measurement: `python tools/artifactprobe.py --hashes tools/spec_hashes.json`
  compares every spec's both hashes against the committed snapshot; the
  snapshot is written when absent, compared when present, and re-baselining is
  a deliberate `rm` (`tools/artifactprobe.py:41-54`). Its red arms (scoping
  off; prune off) each move 828/828 (`asset-forge/docs/artifact-craft.md:206-213`).
- `forge.cli selftest` (`cli.py:409-792`) is the pre-commit net: palette drift,
  determinism, vxa round trip, one-piece rule, engine material ceiling,
  terrain-lattice pinning, spec-says-what-it-builds (with the deliberately
  tripped substitution alarm, `cli.py:643-656`), validation idempotence,
  category resolution, categories.json staleness, biome hosting.
  `tools/buildcheck.py` is the CI regression net with `--category` filtering
  (`buildcheck.py:69-75, 136-150`).

**Rule for everything below:** any phase that adds a row to `spec.PARAMS`, a
kind, or a category MUST run `artifactprobe --hashes` (expect: all existing
specs unchanged on both hashes) AND its two red arms (expect: each arm moves
~all specs — a red arm that stays green means the check is disconnected), plus
`selftest` and the relevant `buildcheck` slice.

---

## 2. Gap analysis against the vision

### (a) Desktop executable/shortcut

Exists: one command, auto-opens the browser, serves a committed static build —
no npm, no install step beyond three pip packages.

Missing for one-click on Windows 11:
1. A launcher script and a Desktop `.lnk` (with icon) — nothing exists.
2. **Single-instance / port handling**: a second launch today dies with
   `OSError: [WinError 10048]` on bind (`server.py:1376` has no handling). The
   right behaviour is: if 127.0.0.1:8731 already answers `/api/kinds`, just
   open the browser.
3. **Dependency preflight**: a missing `scipy` today surfaces as a traceback
   mid-use (`cli.py:123-127` raises only when the one-piece check runs).
4. **Shutdown**: closing the browser leaves the server running; the console
   window is the only handle. Acceptable for a dev tool, but the launcher
   should at least keep a visible console (not `pythonw`) so ctrl-c stays
   available; a tray/`/api/shutdown` affordance is optional polish.
5. **Stale-dist honesty**: the server silently serves whatever `web/dist`
   holds (`server.py:1322-1331`). Fine once Phase 0 lands; a version stamp in
   the built page footer would make "am I running yesterday's UI" answerable.

### (b) Generation via knobs/sliders

Substantially done — the schema-driven panel already exposes every `PARAMS`
row for the selected kind, grouped, with correct ranges, plus seeds/count/
auto-regenerate, health flags on tiles, and the placement/rules/curation tabs.
Residual gaps are small: no UI to author the per-spec `category` override
(read-only today, `SpeciesPanel.tsx:39-53`; the override path is exercised
only by `artifactprobe --arms`, `categories.py:52-57`); `/api/detail` and
`/api/categories` are defined client-side and unused (`api.ts:26, 123`).

### (c) Generation via text / natural language

- **Editing by text is done** (§1.3) and is deliberately local + deterministic.
- **Creation by text does not exist.** Two design options, in order:
  - **C1 — extend the local vocabulary to creation.** `language.py` already
    replaces whole proportion/fin/marking bundles for fish species words
    (`README.md:809-824`). A "new species" grammar = pick kind from a keyword,
    start from `default_spec()`, apply recipes, name it. Deterministic,
    offline, no new dependencies, same failure mode (unknown word says so).
    Ceiling: it will never handle open-ended phrasing.
  - **C2 — an LLM lane (design option only; NOT to be implemented without
    owner sign-off).** Text → an LLM (the Claude API is the obvious backend)
    → a **patch dict against the existing `ui_schema`**, applied through
    `spec.patch` so every value is clamped/validated and out-of-menu choices
    warn (`spec.py:3621`) — the model never writes raw spec JSON, and the
    hash discipline is untouched because `patch` cannot add keys `validate`
    doesn't own. Must be opt-in and visibly labelled: the UI currently
    **promises "nothing leaves this machine"** (`ForgeView.tsx:717-721`), and
    that promise is a standing constraint to be relaxed explicitly, not
    eroded. Open: model choice, where the API key lives, offline fallback
    (C1 remains the fallback), whether responses are logged into `notes` for
    provenance. No dependency is added by this document.

### (d) The 'winning asset' approval flow

Exists: per-species verdicts written into spec files; per-species seed banks;
export-time gating on Path A with by-name reporting of everything held back
(`manifest.py:701-720`); a Library tab built around review.

Gaps, in consumption order:
1. **Approving doesn't publish.** After a verdict, someone must remember
   `export_categories.py` / `export_manifest.py` / `export_banks.py` (+ the
   `kWorldGenVersion` bump rule when the manifest changes,
   `assetmanifest.h:30-36`, `docs/asset-placement-architecture.md` §9). There
   is no "export approved set" action in the app or a single CLI verb.
2. **Path B (craftables) is ungated** — the game loads `draft` canoe/glider by
   hardcoded name straight from the authoring library (§1.5). "Approved" gates
   nothing for exactly the category the vision cares most about.
3. **`categories.json` has no reader** — the seam built for "list the
   craftable items" is consumed by neither the game nor the web app.
4. **The grandfather clause means 'approved' ≠ 'winning'.** 828 of 830 species
   export while `curated: false` — by design (`spec.py:3173-3182`), but the
   vision's "library of approved assets" implies a burn-down: verdicts
   accumulate as Matt reviews, and the export report already counts
   grandfathered separately (`manifest.py:713-715`).
5. **Verdicts are per (species, seed-list), not per kept variant.** `keep` and
   curation are parallel systems: a kept library entry has no verdict field of
   its own (`meta.json` schema, `server.py:341-352`); the bank-seed toggle is
   the de-facto per-variant approval for scatter kinds, and craftables use the
   kept `-0001` entry as the shipping grid (`docs/artifact-craft.md:110-137`).
   This is coherent — but it should be stated as the model rather than left
   implicit (see Open decisions).

### Top 5 gaps, ranked

1. No desktop launcher / single-instance / preflight (a).
2. Approve→publish is manual and multi-step; no one-verb export of the
   approved set (d1).
3. Path B serves draft content from the authoring library; nothing reads
   `categories.json` (d2, d3).
4. No NL→creation; the NL lane's local-only promise vs an LLM backend is an
   undecided fork (c).
5. Frontend source + dist divergence from git; 828 grandfathered verdicts with
   no burn-down mechanism (hygiene + d4).

---

## 3. Phased plan

Each phase is small, independently shippable, and carries gates whose arms can
fail. UE-editor verification is explicitly deferred where the box is
serialized; every phase has non-editor gates.

### Phase 0 — Commit what's running (hygiene, half a day)

Commit the 7 modified `web/src` files, rebuild, commit the new `dist` bundles,
delete the stale tracked ones. No behaviour change intended.

Gates:
- `npm run build` (tsc --noEmit gate included, `web/package.json:8`) exits 0;
  `git status` clean under `asset-forge/web`.
- `python -m forge.cli selftest --quick` PASS.
- Red arm: temporarily rename `web/dist/index.html` → server must fall back to
  the legacy page (`server.py:1325`), proving the dist path is actually the
  one being served; restore.

### Phase 1 — Desktop launch (the shortcut)

Deliverables:
1. `forge.cli app` subcommand (or `tools/launch-asset-forge.ps1`; owner's
   naming call): preflight-import numpy/scipy/pillow with a plain-English
   failure message; probe `http://127.0.0.1:8731/api/kinds` — if it answers,
   open the browser and exit (single-instance); else start `serve`.
2. A generated Desktop shortcut (`.lnk`, WScript.Shell via PowerShell) with an
   icon, targeting the launcher in a visible console window (ctrl-c remains
   the shutdown; document it in the console banner).
3. Optional: `--port` collision fallback (try 8732…) only if the owner wants
   it; otherwise a clear "already running / port busy" message.

Gates (all runnable now, no UE):
- Double-launch arm: run the launcher twice → exactly one `python` process
  serving 8731 (count via `Get-Process`/netstat), second launch opens a
  browser tab and exits 0.
- Kill arm: kill the server; relaunch works; port is rebindable.
- Preflight red arm: `pip uninstall pillow` in a scratch venv → launcher
  prints the named missing package and exits non-zero (no traceback).
- No spec, no PARAMS, no taxonomy touched → `artifactprobe --hashes` trivially
  green; run it anyway as the standing gate.

### Phase 2 — One-verb publish of the approved set

Deliverables:
1. `python tools/publish.py` (or `forge.cli publish`): runs
   `export_categories.py`, `export_manifest.py`, `export_banks.py` in order,
   prints the merged CurationSummary (approved / grandfathered / held-back by
   name, `manifest.py:711-719`), and finishes with `enginecheck.py` +
   `export_categories.py --check` green. It must surface the
   `kWorldGenVersion` obligation loudly whenever `species.vxm` bytes changed.
2. An app affordance (Library tab header): "Publish approved set" → POSTs to a
   new `/api/publish` that shells the same tool and streams its report. Same
   code path as the CLI — one publisher, two callers, per the
   `curated_inputs` precedent (`manifest.py:729-735`).
3. Surface the curation counts (approved/grandfathered/draft/rejected) in the
   Library header so the burn-down is visible.

Gates:
- Round-trip arm: mark a scratch species `rejected` via `/api/curation`,
  publish → its bank files are **deleted** (`export_banks.py:33-44`) and its
  manifest row absent; mark it `approved`, publish → files return,
  `enginecheck` green both times.
- Staleness arm: hand-edit `library/categories.json`, run
  `export_categories.py --check` → FAIL (proves the check bites), then publish
  → PASS.
- Hash arm: publish twice with no verdict changes → `species.vxm` and every
  bank byte-identical (determinism; the manifest comparison in `enginecheck`
  is the counter).
- No PARAMS/taxonomy change → `artifactprobe --hashes` green.

### Phase 3 — Gate the craft path (make 'approved' mean something in-game)

Recommended shape: **extend enforcement-by-omission to Path B** rather than
teaching the engine about curation (the engine deliberately has no status
byte, §1.5, and that property is worth keeping).

Deliverables:
1. Publish (Phase 2) additionally copies **approved** craftables' library
   entries to `out/engine/craft/<name>/<name>-NNNN/tree.vxa` (layout mirroring
   what `ResolveVxaPath` probes, `VoxelAssetBody.cpp:180`), and emits
   `out/engine/craft/categories.json` filtered to approved.
2. UE change (small, one file): default `voxel.AssetBody.LibraryRoot` to
   `<ProjectDir>/../asset-forge/out/engine/craft`, keeping the cvar override
   for dev iteration against the raw library. This is the first and only
   game-side consequence of a verdict, and it is still just a directory path.
3. Optional follow-on (when a real crafting UI exists): the first game-side
   reader of `craft/categories.json` to enumerate craftables instead of
   hardcoded cvar names (`VoxelBoat.cpp:35`).

Gates:
- Omission arm: with canoe/glider still `draft`, publish → `out/engine/craft`
  contains **nothing**; approve canoe (only after the pending in-engine
  capture — see Open decisions), publish → exactly `canoe` appears.
- Path arm (no editor needed): a filesystem check that every
  `grids[].vxa` path in the published `categories.json` exists and parses as
  VXA v3 with the recorded `voxel_mm` (reuse `vxa.read`; refusal on v1/v2 is
  already loud, `vxa.py:170-187`).
- Editor leg (deferred until the box frees up): boat/glider spawn from the new
  root; a draft species logs the tried-paths miss (`VoxelAssetBody.cpp:522`)
  rather than loading.
- UE source touched, so the standing UE gates apply on that lane; asset-forge
  side runs `selftest` + `artifactprobe --hashes` (green — no PARAMS change).

### Phase 4 — Text-to-creation (local first, LLM as an owner-gated option)

Step 1 (local, no new deps): extend `forge/language.py` with a creation
grammar — kind keyword + descriptors → `default_spec()` + existing recipes +
a generated name; expose as a "New from description" affordance beside the
existing Plain speech panel, returning the same
`{understood, ignored, edits}` contract (`schema.ts:345-351`).

Step 2 (only after the Open decisions below are decided): `/api/interpret-llm`
per option C2 in §2(c) — patch-dict-only output, applied through `spec.patch`,
opt-in toggle, labelled as leaving the machine, C1 as the offline fallback.

Gates:
- DEAD-arm rule (the `fishprobe` precedent, `README.md:826-846`): every
  creation recipe must move a measurable the probe suite can check; a recipe
  that produces `default_spec()` unchanged is a failure.
- Unknown-input arm: a nonsense sentence must return a non-empty `ignored`
  list and zero edits — never a silent default species.
- Hash-isolation arms (this phase is the likeliest to touch `PARAMS`): if any
  row is added, it must be added under `KIND_SCOPED_PARAMS` for its kind, then
  `artifactprobe --hashes` → all pre-existing specs unchanged on BOTH hashes,
  and both red arms still move ~all specs. If a row cannot be kind-scoped, the
  phase stops and the reseed goes to the owner as its own decision
  (`spec.py:3546-3580` records why).
- LLM lane (if built): a recorded fixture arm — the same prompt fixture must
  produce a patch that `spec.patch` accepts with zero warnings, and a
  deliberately out-of-menu fixture must produce the substitution warning
  (`cli.py:643-656` proves the alarm works; this arm proves the lane hits it).

### Phase ordering rationale

0 before everything (the tool being formalized must be the tool in git).
1 is pure launch ergonomics with no game coupling — immediate daily value.
2 before 3 because 3's publish step is 2's publisher with one more output.
4 last because it is the only phase with an undecided external dependency and
the only one likely to touch `PARAMS`.

---

## 4. Open decisions for Matt

1. **Naming.** The desktop entry's name/icon ("Asset Forge"?) and whether the
   launcher is `forge.cli app` or a `tools/*.ps1` + `.lnk`.
   > **RULED 2026-09-05 (owner):** Name is **"Asset Forge"**; icon is "a cool
   > forge icon stylized with voxels" (generated programmatically,
   > `asset-forge/tools/make_icon.py` → `asset-forge.ico`). No owner opinion on
   > cli-vs-tools; implemented as `asset-forge/tools/launch-asset-forge.ps1` +
   > `.lnk` — a new file rather than a `forge.cli` subcommand, so the launcher
   > does not entangle with in-flight `cli.py` lane work.
2. **Desktop entry form.** Script+shortcut (recommended: zero packaging,
   Python already works) vs a packaged exe (PyInstaller — adds a build
   artifact, a ~hundred-MB binary with numpy/scipy, and an update problem).
   This plan assumes script+shortcut.
   > **RULED 2026-09-05 (owner):** Script + shortcut. No packaged exe.
3. **NL backend.** (a) Local-only creation grammar (C1) and stop; or (b) add
   the Claude-API lane (C2). If (b): which model, where the key lives (env
   var? file outside the repo?), whether the "nothing leaves this machine"
   banner becomes a per-panel distinction, and whether prompts/responses are
   recorded into spec `notes` for provenance. No code or dependency moves
   until this is decided.
   > **RULED 2026-09-05 (owner):** Deferred — build **no** LLM lane now (a
   > subscription-auth question is being researched separately). The local
   > creation grammar (C1) remains the future Phase 4 step; neither lane is in
   > the current implementation task's scope.
4. **Where approval state lives.** This plan keeps it where it is — in the
   spec file, hashed out of identity (`spec.py:3458-3474`) — and extends
   enforcement-by-omission to Path B. The alternative (a game-side
   curation-aware reader of `categories.json`) teaches the engine about
   verdicts; say so explicitly if that is wanted, because it reverses a
   deliberate property.
   > **RULED 2026-09-05 (owner):** Approval enforcement stays by-omission; the
   > engine keeps no status byte.
5. **Per-variant verdicts.** Today "winning" = species approved + seed on its
   bank list (+ kept entry for craft). Is that the model, or do individual
   kept variants need their own approve/reject (a `meta.json` field +
   LibraryInspector buttons)? Cheap to add, but it creates a second verdict
   surface that publish must then reconcile.
   > **RULED 2026-09-05 (owner):** The verdict model stays species+seed. No
   > per-variant verdicts.
6. **Grandfather burn-down.** Leave 828 species grandfathered-approved
   indefinitely, or set a policy (e.g., LibraryView defaults to the
   "Never reviewed" filter; publish prints the count until zero)? Nothing
   should auto-demote them — that would unpublish the world (`spec.py:3173-3182`).
   > **RULED 2026-09-05 (owner):** The owner will review ALL 828 grandfathered
   > species and approve them explicitly. The plan's policy is adopted:
   > LibraryView defaults to the "Never reviewed" filter, and the
   > remaining-unreviewed count is surfaced prominently — in the UI, and in
   > every publish/export summary (`CurationSummary.lines` already prints the
   > grandfathered count) — until it reaches zero. Nothing auto-demotes.
7. **Canoe/glider approval.** Both are `draft` pending in-engine capture, and
   Phase 3's omission arm depends on that verdict being made honestly — the
   capture needs the editor box, currently serialized behind water-gate legs.
   Related craft decisions (pivot, pitch, strake, sail terracing) are already
   queued for the owner in `asset-forge/docs/artifact-craft.md:168-194` and are
   not re-opened here.
   > **RULED 2026-09-05 (owner):** "Fine in the canoe and glider" — both
   > curation verdicts set to `approved` (written into the spec files exactly
   > as `/api/curation` would). The in-engine capture still happens later as
   > the water plan's D-gate; the verdict is the owner's session ruling, not
   > that capture.
8. **Single-user vs multi-machine.** Loopback, no auth, files written in the
   repo checkout is the current model and this plan keeps it. If a second
   machine (or phone-first review) becomes real, that is an auth + sync design
   problem of its own — flagged only; nothing in Phases 0-4 forecloses it.
   > **RULED 2026-09-05 (owner):** Single-user stays. Loopback, no auth, files
   > in the repo checkout.

9. **Category naming.** (Recorded for completeness — raised alongside the
   above.) The category taxonomy as it stands: `environment` / `creature` /
   `craftable`.
   > **RULED 2026-09-05 (owner):** Approved as-is. No code change.

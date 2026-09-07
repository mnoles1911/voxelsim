import * as React from "react";
import {
  Archive, ArrowRight, Dices, Flag, Hammer, MessageSquareText, Plus, RefreshCw, Save, Sparkles, Undo2, Wand2,
} from "lucide-react";
import type { World } from "../App";
import { api, forgeApi } from "../lib/api";
import type { JobProgress, Kind, TileState, UiParam, UiSchema } from "../lib/schema";
import { CATEGORIES, CATEGORY_LABEL } from "../lib/schema";
import { getPath, setPath } from "../lib/schema";
import { groupLabel } from "../lib/taxonomy";
import { kindIcon } from "../lib/kindIcons";
import { Badge } from "./ui/badge";
import { Button } from "./ui/button";
import { Checkbox } from "./ui/checkbox";
import { Dialog, DialogContent, DialogTitle } from "./ui/dialog";
import { FloatingPanel } from "./ui/floating-panel";
import { Input, Textarea } from "./ui/input";
import {
  Select, SelectContent, SelectGroup, SelectItem, SelectLabel, SelectTrigger, SelectValue,
} from "./ui/select";
import { useToast } from "./ui/toast";
import { VoxelCanvas } from "./VariantViewer";
import { cn } from "../lib/cn";

/* THE FORGE: stages 1 and 2 of the owner's workflow -- generate a 3D asset
 * within a class, then fine-tune it against the live preview. A port of the
 * old forge/web/app.js authoring surface onto the React stack:
 *
 *   - the parameter panel is GENERATED from /api/schema (forge/spec.py's
 *     P(...) rows), grouped and clamped exactly as the schema says, so a new
 *     kind's controls appear with zero UI work;
 *   - generation posts /api/generate and polls /api/job (the server renders
 *     tiles in a worker pool; deterministic (spec, seed) means everything can
 *     be regenerated on demand);
 *   - "Keep to library" (/api/keep) is the hinge into stage 3, and the strip
 *     at the top hands off to stage 4 (placement) for the current species;
 *   - the plain-language box is forge/language.py via /api/interpret, and
 *     "new from description" is /api/create -- both fully LOCAL. The privacy
 *     promise is PER PANEL (owner ruling 2026-09-05): the one exception is
 *     the labelled, per-use "Ask Claude" button (/api/interpret-llm), which
 *     sends that text to Claude on the owner's subscription.
 */

const DEFAULT_SPECIES: Record<string, string> = {
  tree: "temperate-oak", bush: "bramble-thicket", rock: "granite-boulder",
  grass: "meadow-grass", reed: "water-reed", flower: "meadow-daisy",
  fish: "brown-trout", cetacean: "bottlenose-dolphin", bird: "european-robin",
  quadruped: "red-fox", artifact: "canoe",
};

/** Groups worth opening by default; the rest start folded. */
const OPEN_GROUPS = new Set(["general", "crown", "trunk", "rock", "tuft", "fish", "bird", "quad",
                             "artifact"]);

type Spec = Record<string, unknown>;

export interface ForgeRequest {
  spec: Spec;
  seedStart: number;
  n: number; // nonce so identical requests still fire
}

export function ForgeView({
  world, request, onOpenPlacement,
}: {
  world: World;
  request: ForgeRequest | null;
  onOpenPlacement: (species: string) => void;
}) {
  const toast = useToast();
  const readyKinds = world.kinds.filter((k) => k.ready);
  /* The kind menu, grouped by CATEGORY. Eleven kinds flat gave a reader no way
   * to see that the newest one is neither scenery nor an animal. Order follows
   * `CATEGORIES` (the server's own order) and anything the server hands back
   * with an unknown category still appears, under "other" -- a kind that
   * vanished from the menu because its category was mistyped would be a far
   * worse failure than an ugly heading. */
  const kindsByCategory = React.useMemo(() => {
    const seen = new Set<string>();
    const groups: { key: string; label: string; kinds: Kind[] }[] = [];
    for (const c of CATEGORIES) {
      const members = readyKinds.filter((k) => k.category === c);
      members.forEach((k) => seen.add(k.key));
      if (members.length) groups.push({ key: c, label: CATEGORY_LABEL[c] ?? c, kinds: members });
    }
    const rest = readyKinds.filter((k) => !seen.has(k.key));
    if (rest.length) groups.push({ key: "other", label: "Other", kinds: rest });
    return groups;
  }, [readyKinds]);
  const [kind, setKind] = React.useState<string>(readyKinds[0]?.key ?? "tree");
  /* THE ENTRY PATH (owner directive 2026-09-05): category -> kind -> species,
   * three explicit selects, so the left panel is for EXISTING content --
   * tweak it with sliders and the local vocabulary. Net-new types have their
   * own dedicated flow (CreateWizard). */
  const [category, setCategory] = React.useState<string>(
    readyKinds[0]?.category ?? "environment");
  const [wizardOpen, setWizardOpen] = React.useState(false);
  const [schema, setSchema] = React.useState<UiSchema | null>(null);
  const [spec, setSpec] = React.useState<Spec | null>(null);
  const [saved, setSaved] = React.useState<Spec | null>(null);
  const [hash, setHash] = React.useState("");
  const [rev, setRev] = React.useState(0); // bump to re-render after mutating `spec` in place

  const [seedStart, setSeedStart] = React.useState(1);
  const [count, setCount] = React.useState(12);
  const [auto, setAuto] = React.useState(true);

  const [job, setJob] = React.useState<{ id: string; seeds: number[]; t0: number } | null>(null);
  const [progress, setProgress] = React.useState<JobProgress | null>(null);
  const [detailSeed, setDetailSeed] = React.useState<number | null>(null);
  const pollRef = React.useRef<number | null>(null);

  const speciesName = String(getPath(spec, "name") ?? "");
  const specRow = world.specs.find((s) => s.name === speciesName);
  /* Owner directive 2026-09-05: placement applies to environment and
   * creature assets ONLY. Vehicles (category craftable) get no placement
   * UI at all -- absent, not disabled. CATEGORY-driven (the server's
   * resolved category; the kind's default for an unsaved spec), never a
   * hard-coded kind list, so a future vehicles member inherits it. This
   * mirrors an engine truth, not a UI preference: ADR-0010 puts entity
   * kinds outside world composition and manifest.species_record refuses
   * them from species.vxm by name. */
  const isVehicleCat = specRow?.category
    ? specRow.category === "craftable"
    : world.kinds.find((k) => k.key === kind)?.category === "craftable";
  const keptIds = React.useMemo(() => new Set(world.library.map((e) => e.id)), [world.library]);
  const craftGroups = [...new Set(world.specs.filter((s) => s.category === "craftable")
    .map((s) => s.subcategory ?? "ungrouped"))].sort();
  const craftGroup = String(spec?.subcategory ?? "ungrouped");
  const specsOfKind = world.specs.filter((s) => category === "craftable"
    ? s.category === category && (s.subcategory ?? "ungrouped") === craftGroup
    : s.kind === kind && s.category === category);

  /* --- loading ----------------------------------------------------------- */

  const stopPolling = React.useCallback(() => {
    if (pollRef.current != null) {
      window.clearInterval(pollRef.current);
      pollRef.current = null;
    }
  }, []);
  React.useEffect(() => stopPolling, [stopPolling]);

  const generate = React.useCallback(
    async (fromSpec: Spec, start: number, n: number) => {
      try {
        const r = await forgeApi.generate(fromSpec, start, n);
        setHash(r.hash);
        for (const w of r.warnings ?? []) toast.error(w);
        const t0 = performance.now();
        setJob({ id: r.job, seeds: r.seeds, t0 });
        setProgress(null);
        setDetailSeed(null);
        stopPolling();
        pollRef.current = window.setInterval(async () => {
          try {
            const p = await forgeApi.job(r.job);
            setProgress(p);
            if (p.done >= p.total) stopPolling();
          } catch {
            stopPolling();
          }
        }, 350);
      } catch (e) {
        toast.error("Generate failed: " + String(e));
      }
    },
    [stopPolling, toast],
  );

  const loadSpec = React.useCallback(
    async (name: string, { gen = true } = {}) => {
      try {
        const r = await forgeApi.spec(name);
        setSpec(r.spec);
        setSaved(JSON.parse(JSON.stringify(r.spec)));
        setHash(r.hash);
        setRev((v) => v + 1);
        if (gen) void generate(r.spec, seedStart, count);
      } catch (e) {
        toast.error(String(e));
      }
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [generate, toast],
  );

  const loadKind = React.useCallback(
    async (k: string, { gen = true } = {}) => {
      setKind(k);
      setCategory(world.kinds.find((x) => x.key === k)?.category ?? "other");
      const sch = await forgeApi.schema(k);
      setSchema(sch);
      const of = world.specs.filter((s) => s.kind === k);
      const first = of.find((s) => s.name === DEFAULT_SPECIES[k]) ?? of[0];
      if (first) await loadSpec(first.name, { gen });
      else {
        setSpec(null);
        setSaved(null);
      }
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [loadSpec, world.specs, world.kinds],
  );

  /* Load a kind's schema and adopt an already-built spec (the creation
   * flow's landing: the server saved the draft; the gallery reviews it). */
  const loadKindShellFor = React.useCallback(
    async (k: string, created: Spec) => {
      setKind(k);
      setCategory(world.kinds.find((x) => x.key === k)?.category ?? "other");
      setSchema(await forgeApi.schema(k));
      setSpec(created);
      setSaved(JSON.parse(JSON.stringify(created)));
      setRev((v) => v + 1);
    },
    [world.kinds],
  );

  /* boot once */
  const booted = React.useRef(false);
  React.useEffect(() => {
    if (booted.current) return;
    booted.current = true;
    void loadKind(kind, { gen: true });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  /* "More like this" arriving from the library */
  React.useEffect(() => {
    if (!request) return;
    void (async () => {
      const k = String(getPath(request.spec, "kind") ?? "tree");
      setKind(k);
      setSchema(await forgeApi.schema(k));
      setSpec(request.spec);
      setSaved(JSON.parse(JSON.stringify(request.spec)));
      setSeedStart(request.seedStart);
      setRev((v) => v + 1);
      void generate(request.spec, request.seedStart, count);
    })();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [request?.n]);

  /* --- parameter edits ---------------------------------------------------- */

  const editParam = (p: UiParam, value: unknown) => {
    if (!spec) return;
    setPath(spec, p.path, value);
    setRev((v) => v + 1);
  };
  const commitParam = () => {
    if (auto && spec) void generate(spec, seedStart, count);
  };

  const revert = () => {
    if (!saved) return;
    const copy = JSON.parse(JSON.stringify(saved));
    setSpec(copy);
    setRev((v) => v + 1);
    void generate(copy, seedStart, count);
  };

  const saveSpecFile = async () => {
    if (!spec) return;
    try {
      const r = await forgeApi.saveSpec(spec);
      for (const w of r.warnings ?? []) toast.error(w);
      setSaved(JSON.parse(JSON.stringify(spec)));
      setRev((v) => v + 1);
      await world.refreshSpecs();
      toast.ok("Saved specs/" + r.saved);
    } catch (e) {
      toast.error("Save failed: " + String(e));
    }
  };

  /* --- keeping (stage 3) --------------------------------------------------- */

  const keptThisSession = React.useRef(new Set<string>());
  const entryId = (seed: number) => speciesName + "-" + String(seed).padStart(4, "0");
  const isKept = (seed: number) => keptIds.has(entryId(seed)) || keptThisSession.current.has(entryId(seed));

  const keep = async (seed: number, { quiet = false } = {}) => {
    if (!spec) return false;
    if (isKept(seed)) return true;
    try {
      const m = await forgeApi.keep(spec, seed);
      keptThisSession.current.add(m.id);
      if (!quiet) {
        toast.ok("Kept " + m.id + " to the library");
        await world.refreshLibrary();
      }
      return true;
    } catch (e) {
      if (!quiet) toast.error("Keep failed: " + String(e));
      return false;
    }
  };

  const keepAllClean = async () => {
    if (!progress) return;
    const clean = Object.entries(progress.tiles)
      .filter(([s, t]) => t.ready && !t.error && (t.problems ?? []).length === 0 && !isKept(Number(s)))
      .map(([s]) => Number(s));
    if (clean.length === 0) return toast.ok("Nothing new to keep");
    let saved2 = 0;
    for (const s of clean) if (await keep(s, { quiet: true })) saved2++;
    await world.refreshLibrary();
    const flagged = Object.values(progress.tiles).filter((t) => (t.problems ?? []).length > 0).length;
    toast.ok("Kept " + saved2 + (flagged ? " -- skipped " + flagged + " flagged (look before keeping)" : ""));
  };

  /* bank-seed toggling: the seed's place in the PUBLISHED bank, distinct from
   * "keep to library" -- same law as the curation bar. */
  const toggleBankSeed = async (seed: number) => {
    if (!specRow) return toast.error("Save the spec first -- the bank belongs to specs/" + speciesName + ".json");
    const c = specRow.curation;
    const has = c.seeds.includes(seed);
    if (has && c.seeds.length === 1) return toast.error("A published species needs at least one bank seed.");
    const seeds = has ? c.seeds.filter((s) => s !== seed) : [...c.seeds, seed].sort((a, b) => a - b);
    try {
      const r = await api.saveCuration(specRow.name, c.status, seeds, c.notes);
      world.patchSpec(specRow.name, { curation: { ...r.curation, curated: true } });
    } catch (e) {
      toast.error(String(e));
    }
  };

  /* --- render -------------------------------------------------------------- */

  const kindMeta = world.kinds.find((k) => k.key === kind);
  const done = progress?.done ?? 0;
  const total = progress?.total ?? job?.seeds.length ?? 0;
  const running = job != null && done < total;
  const secs = job ? ((performance.now() - job.t0) / 1000).toFixed(1) : "0";

  return (
    <div className="flex min-h-0 flex-1">
      {/* parameter rail (stage 2 lives here) */}
      <aside className="flex min-h-0 w-[380px] shrink-0 flex-col border-r-2 border-stone-950 bg-stone-850">
        <div className="mortar-b flex flex-col gap-2 p-3">
          {/* Existing content: category -> kind -> species. */}
          <div className="grid grid-cols-2 gap-2">
            <Select
              value={category}
              onValueChange={(c) => {
                setCategory(c);
                const first = kindsByCategory.find((g) => g.key === c)?.kinds[0];
                if (first && first.key !== kind) void loadKind(first.key);
              }}
            >
              <SelectTrigger><SelectValue /></SelectTrigger>
              <SelectContent>
                {kindsByCategory.map((g) => (
                  <SelectItem key={g.key} value={g.key}>{g.label}</SelectItem>
                ))}
              </SelectContent>
            </Select>
            {category === "craftable" ? (
              <Select value={craftGroup} onValueChange={async (group) => {
                const first = world.specs.find((s) => s.category === "craftable"
                  && (s.subcategory ?? "ungrouped") === group);
                if (!first) return;
                setKind(first.kind);
                setSchema(await forgeApi.schema(first.kind));
                await loadSpec(first.name);
              }}>
                <SelectTrigger aria-label="Craftable subcategory"><SelectValue /></SelectTrigger>
                <SelectContent>
                  {craftGroups.map((group) => (
                    <SelectItem key={group} value={group}>
                      {groupLabel(group)} ({world.specs.filter((s) => s.category === "craftable"
                        && (s.subcategory ?? "ungrouped") === group).length})
                    </SelectItem>
                  ))}
                </SelectContent>
              </Select>
            ) : (
            <Select value={kind} onValueChange={(k) => void loadKind(k)}>
              <SelectTrigger><SelectValue /></SelectTrigger>
              <SelectContent>
                {(kindsByCategory.find((g) => g.key === category)?.kinds ?? readyKinds).map((k) => (
                  <SelectItem key={k.key} value={k.key}>
                    {k.label} ({k.species})
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
            )}
          </div>
          <Select
            value={specsOfKind.some((s) => s.name === speciesName) ? speciesName : undefined}
            onValueChange={(n) => void loadSpec(n)}
          >
            <SelectTrigger><SelectValue placeholder="species…" /></SelectTrigger>
            <SelectContent>
              {/* Grouped by sub-category label when authored; a label is a
                * grouping over the kind's generator, never a generator. */}
              {[...new Set(specsOfKind.map((s) => s.subcategory ?? ""))].sort().map((sub) => {
                const members = specsOfKind.filter((s) => (s.subcategory ?? "") === sub);
                if (!sub) {
                  return members.map((s) => (
                    <SelectItem key={s.name} value={s.name}>{s.name}</SelectItem>
                  ));
                }
                return (
                  <SelectGroup key={sub}>
                    <SelectLabel>{groupLabel(sub)}</SelectLabel>
                    {members.map((s) => (
                      <SelectItem key={s.name} value={s.name}>{s.name}</SelectItem>
                    ))}
                  </SelectGroup>
                );
              })}
            </SelectContent>
          </Select>
          {kindMeta && (
            <p className="flex flex-wrap items-center gap-1.5 text-xs text-parch-500">
              {/* WHAT the loaded species is, and WHO said so. `via: "spec"`
                * means a human overrode the kind's default and that is worth
                * seeing; `via: "illegible"` means nothing can classify it and
                * no index will list it, which is worth seeing loudly. */}
              {specRow?.category && (
                <Badge variant={specRow.category === "craftable" ? "gold" : "outline"}>
                  {CATEGORY_LABEL[specRow.category] ?? specRow.category}
                  {specRow.category_via === "spec" ? " (per spec)" : ""}
                </Badge>
              )}
              {specRow && !specRow.category && (
                <Badge variant="rust">no category — listed nowhere</Badge>
              )}
              <span>{kindMeta.blurb}</span>
            </p>
          )}
          <div className="flex items-center gap-2">
            <Input
              className="h-7 font-mono text-xs"
              value={speciesName}
              onChange={(e) => spec && (setPath(spec, "name", e.target.value), setRev((v) => v + 1))}
              placeholder="species name"
            />
            <span className="font-mono text-[10px] text-parch-500">{hash}</span>
          </div>
          <div className="flex gap-2">
            <Button size="sm" variant="gold" onClick={() => void saveSpecFile()} disabled={!spec}>
              <Save className="h-3.5 w-3.5" /> Save spec
            </Button>
            <Button size="sm" variant="ghost" onClick={revert} disabled={!spec}>
              <Undo2 className="h-3.5 w-3.5" /> Revert
            </Button>
            <Button
              size="sm"
              variant="moss"
              className="ml-auto"
              onClick={() => setWizardOpen(true)}
              title="Create a net-new asset type from a description (guided; routed to Claude)"
            >
              <Plus className="h-3.5 w-3.5" /> New asset type
            </Button>
          </div>
        </div>

        <div className="min-h-0 flex-1 overflow-y-auto p-2" key={"params-" + kind + "-" + speciesName}>
          {schema && spec ? (
            <ParamPanel schema={schema} spec={spec} saved={saved} rev={rev} onEdit={editParam} onCommit={commitParam} />
          ) : (
            <div className="p-4 text-sm text-parch-500">
              {schema ? "No " + (kindMeta?.label ?? "").toLowerCase() + " authored yet." : "Loading the schema…"}
            </div>
          )}
        </div>

        {wizardOpen && (
          <CreateWizard
            world={world}
            onClose={() => setWizardOpen(false)}
            onCreated={async (k, created, name, summary) => {
              setWizardOpen(false);
              await world.refreshSpecs();          // the server saved the draft spec
              await loadKindShellFor(k, created);
              toast.ok(summary ?? "New " + k + " species '" + name + "' (draft; keep seeds to publish)");
              void generate(created, seedStart, count);
            }}
          />
        )}
        <AskPanel
          spec={spec}
          onApplied={(next, summary) => {
            setSpec(next);
            setRev((v) => v + 1);
            toast.ok(summary);
            void generate(next, seedStart, count);
          }}
        />
      </aside>

      {/* gallery (stage 1's output) */}
      <div className="flex min-h-0 min-w-0 flex-1 flex-col">
        <WorkflowStrip
          canPlace={!!specRow && !isVehicleCat}
          species={speciesName}
          onPlace={() => specRow && onOpenPlacement(specRow.name)}
          noPlacement={isVehicleCat}
        />

        <div className="mortar-b flex flex-wrap items-center gap-2 bg-stone-850 px-3 py-2">
          <Button variant="gold" size="sm" onClick={() => spec && void generate(spec, seedStart, count)} disabled={!spec}>
            <Hammer className="h-4 w-4" /> Generate
          </Button>
          <Button
            variant="default"
            size="sm"
            onClick={() => {
              const s = Math.floor(Math.random() * 90000) + 1;
              setSeedStart(s);
              if (spec) void generate(spec, s, count);
            }}
            disabled={!spec}
            title="New random seed block"
          >
            <Dices className="h-4 w-4" /> Reroll
          </Button>
          <Button
            variant="ghost"
            size="sm"
            onClick={() => {
              if (!spec || !job) return;
              const next = (job.seeds[job.seeds.length - 1] ?? 0) + 1;
              void generate(spec, next, count); // fresh job, next seed block
            }}
            disabled={!spec || !job || running}
            title="Next block of seeds with the same parameters"
          >
            <Plus className="h-4 w-4" /> More seeds
          </Button>
          <label className="flex items-center gap-1.5 text-xs text-parch-400">
            from seed
            <Input type="number" min={0} className="h-7 w-20 text-xs" value={seedStart}
              onChange={(e) => setSeedStart(Math.max(0, Number(e.target.value) || 0))} />
          </label>
          <label className="flex items-center gap-1.5 text-xs text-parch-400">
            count
            <Input type="number" min={1} max={200} className="h-7 w-16 text-xs" value={count}
              onChange={(e) => setCount(Math.max(1, Math.min(200, Number(e.target.value) || 12)))} />
          </label>
          <label className="flex cursor-pointer items-center gap-1.5 text-xs text-parch-400">
            <Checkbox checked={auto} onCheckedChange={(v) => setAuto(v === true)} />
            auto-forge on release
          </label>
          <div className="ml-auto flex items-center gap-2">
            <Button variant="moss" size="sm" onClick={() => void keepAllClean()} disabled={!progress || running}>
              <Archive className="h-3.5 w-3.5" /> Keep all clean
            </Button>
          </div>
        </div>

        {/* progress: the visible generation state */}
        <div className="mortar-b bg-stone-900 px-3 py-1.5">
          <div className="flex items-center gap-3">
            <div className="bevel-down h-3 flex-1 bg-stone-950">
              <div
                className="h-full bg-gold-600 transition-[width] duration-300"
                style={{ width: total ? (done / total) * 100 + "%" : "0%" }}
              />
            </div>
            <span className="w-44 whitespace-nowrap text-right font-mono text-xs text-parch-400">
              {job == null
                ? "the anvil is idle"
                : running
                  ? done + "/" + total + " forged · " + secs + "s"
                  : total + " " + (kindMeta?.label ?? "assets").toLowerCase() + " in " + secs + "s"}
            </span>
          </div>
        </div>

        <div className="min-h-0 flex-1 overflow-y-auto p-3">
          {job == null ? (
            <div className="flex h-full items-center justify-center text-center font-display text-parch-500">
              Pick a species and strike Generate.
            </div>
          ) : (
            <div className="grid grid-cols-[repeat(auto-fill,minmax(180px,1fr))] gap-3">
              {job.seeds.map((seed) => (
                <SeedTile
                  key={job.id + "-" + seed}
                  job={job.id}
                  seed={seed}
                  tile={progress?.tiles[String(seed)]}
                  kept={isKept(seed)}
                  inBank={specRow?.curation.seeds.includes(seed) ?? false}
                  onOpen={() => setDetailSeed(seed)}
                  onKeep={() => void keep(seed)}
                  onBank={() => void toggleBankSeed(seed)}
                />
              ))}
            </div>
          )}
        </div>
      </div>

      {detailSeed != null && job && (
        <SeedDetail
          world={world}
          job={job.id}
          seed={detailSeed}
          species={speciesName}
          tile={progress?.tiles[String(detailSeed)]}
          kept={isKept(detailSeed)}
          onKeep={() => void keep(detailSeed)}
          onPlace={specRow && !isVehicleCat ? () => onOpenPlacement(specRow.name) : undefined}
          onClose={() => setDetailSeed(null)}
        />
      )}
    </div>
  );
}

/* --- the four-stage strip ------------------------------------------------ */

function WorkflowStrip({
  canPlace, species, onPlace, noPlacement = false,
}: {
  canPlace: boolean;
  species: string;
  onPlace: () => void;
  /** Vehicles (category craftable) end at stage 3: ADR-0010 puts entity
   *  kinds outside world composition -- no bank, no biome weight, no
   *  manifest row (manifest.species_record refuses them by name) -- so
   *  placement is not greyed out here, it does not exist. */
  noPlacement?: boolean;
}) {
  const Step = ({ n, label }: { n: number; label: string }) => (
    <span className="flex items-center gap-1.5 text-parch-400">
      <span className="chamfer-sm bevel-up flex h-5 w-5 items-center justify-center bg-stone-600 font-mono text-[11px] text-gold-400">
        {n}
      </span>
      <span className="font-display text-xs uppercase tracking-widest">{label}</span>
    </span>
  );
  return (
    <div className="mortar-b flex items-center gap-3 bg-stone-850 px-3 py-1.5">
      <Step n={1} label="Generate" />
      <ArrowRight className="h-3.5 w-3.5 text-parch-600" />
      <Step n={2} label="Fine-tune" />
      <ArrowRight className="h-3.5 w-3.5 text-parch-600" />
      <Step n={3} label="Keep to library" />
      {noPlacement ? (
        <span className="font-mono text-[11px] text-parch-600">
          · craftable items spawn as entities
        </span>
      ) : (<>
      <ArrowRight className="h-3.5 w-3.5 text-parch-600" />
      <button
        onClick={onPlace}
        disabled={!canPlace}
        className={cn(
          "flex items-center gap-1.5",
          canPlace ? "text-gold-400 hover:text-gold-500" : "pointer-events-none text-parch-600",
        )}
        title={canPlace ? "Open the placement panel for " + species : "Save the spec first"}
      >
        <span className="chamfer-sm bevel-up flex h-5 w-5 items-center justify-center bg-stone-600 font-mono text-[11px]">4</span>
        <span className="font-display text-xs uppercase tracking-widest">Placement</span>
        <ArrowRight className="h-3.5 w-3.5" />
      </button>
      </>)}
    </div>
  );
}

/* --- schema-driven parameter panel ---------------------------------------- */

function ParamPanel({
  schema, spec, saved, rev, onEdit, onCommit,
}: {
  schema: UiSchema;
  spec: Spec;
  saved: Spec | null;
  rev: number;
  onEdit: (p: UiParam, v: unknown) => void;
  onCommit: () => void;
}) {
  void rev; // rerender trigger; the spec object itself is mutated in place
  const byGroup = new Map<string, UiParam[]>();
  for (const p of schema.params) {
    if (!byGroup.has(p.group)) byGroup.set(p.group, []);
    byGroup.get(p.group)!.push(p);
  }
  return (
    <div className="flex flex-col gap-1.5">
      {[...byGroup.entries()].map(([group, params]) => (
        <details key={group} open={OPEN_GROUPS.has(group)} className="chamfer-sm bg-stone-800">
          <summary className="cursor-pointer select-none px-2.5 py-1.5 font-display text-xs uppercase tracking-widest text-parch-400 hover:text-parch-200">
            {group}
          </summary>
          <div className="flex flex-col gap-2 px-2.5 pb-2.5">
            {params.map((p) => (
              <ParamControl
                key={p.path}
                p={p.choices_by_category ? {...p, choices:p.choices_by_category[String(spec.category ?? p.default_category)] ?? p.choices} : p}
                value={getPath(spec, p.path)}
                changed={saved != null && JSON.stringify(getPath(spec, p.path)) !== JSON.stringify(getPath(saved, p.path))}
                onEdit={(v) => onEdit(p, v)}
                onCommit={onCommit}
              />
            ))}
          </div>
        </details>
      ))}
    </div>
  );
}

function ParamControl({
  p, value, changed, onEdit, onCommit,
}: {
  p: UiParam;
  value: unknown;
  changed: boolean;
  onEdit: (v: unknown) => void;
  onCommit: () => void;
}) {
  const decimals = String(p.step).split(".")[1]?.length ?? 0;
  const fmt = (v: unknown) => (p.kind === "int" ? String(v) : Number(v).toFixed(decimals));

  if (p.kind === "bool")
    return (
      <label
        className={cn("flex cursor-pointer items-center gap-2 text-sm", changed ? "text-gold-400" : "text-parch-200")}
        title={p.help}
      >
        <Checkbox
          checked={value === true}
          onCheckedChange={(v) => {
            onEdit(v === true);
            onCommit();
          }}
        />
        {p.label}
      </label>
    );

  if (p.kind === "choice")
    return (
      <div title={p.help}>
        <div className={cn("mb-0.5 text-xs", changed ? "text-gold-400" : "text-parch-400")}>{p.label}</div>
        <Select
          value={String(value ?? "")}
          onValueChange={(v) => {
            onEdit(v);
            onCommit();
          }}
        >
          <SelectTrigger className="h-7 text-xs"><SelectValue /></SelectTrigger>
          <SelectContent>
            {p.choices.map((c) => (
              <SelectItem key={c} value={c}>{p.path === "resolution_cm" ? `${Number(c)*10} mm` : c}</SelectItem>
            ))}
          </SelectContent>
        </Select>
      </div>
    );

  if (p.kind === "text")
    return (
      <div title={p.help}>
        <div className={cn("mb-0.5 text-xs", changed ? "text-gold-400" : "text-parch-400")}>{p.label}</div>
        <Input
          className="h-7 text-xs"
          value={String(value ?? "")}
          onChange={(e) => onEdit(e.target.value)}
          onBlur={onCommit}
        />
      </div>
    );

  /* int / float: a slider clamped to the schema's range and step */
  return (
    <div title={p.help}>
      <div className="mb-0.5 flex items-baseline justify-between">
        <span className={cn("text-xs", changed ? "text-gold-400" : "text-parch-400")}>{p.label}</span>
        <span className="font-mono text-xs text-parch-300">{fmt(value ?? p.default)}</span>
      </div>
      <input
        type="range"
        className="vox-range w-full"
        min={p.lo}
        max={p.hi}
        step={p.kind === "int" ? Math.max(1, p.step) : p.step}
        value={Number(value ?? p.default)}
        onChange={(e) => onEdit(p.kind === "int" ? Math.round(Number(e.target.value)) : Number(e.target.value))}
        onPointerUp={onCommit}
        onKeyUp={(e) => (e.key === "ArrowLeft" || e.key === "ArrowRight") && onCommit()}
      />
    </div>
  );
}

/* --- the creation wizard (owner directive 2026-09-05) ----------------------
 *
 * The dedicated flow for NET-NEW asset types, four steps in order:
 *   1. describe it   2. pick the taxonomy (category)   3. pick or create a
 *   sub-category   4. name it -- then the description routes to CLAUDE (the
 * subscription lane; the local grammar answers if claude is missing,
 * labelled) and the seed variants generate for review.
 *
 * THE HONESTY RULE ON STEP 3: kinds are procedural GENERATOR CODE; creating
 * a sub-category cannot conjure a generator. A new sub-category is a
 * GROUPING LABEL over an existing generator ("based on"), stored in the
 * spec's hash-excluded `subcategory` field, and a truly-new-generator wish
 * is recorded as a TODO in the spec's notes -- never pretended. */

function CreateWizard({
  world, onClose, onCreated,
}: {
  world: World;
  onClose: () => void;
  onCreated: (kind: string, spec: Spec, name: string, summary?: string) => void | Promise<void>;
}) {
  const [desc, setDesc] = React.useState("");
  const [category, setCategory] = React.useState<string>("creature");
  const [pick, setPick] = React.useState<string>("");     // kind key | "sub:<label>:<kind>" | "new"
  const [newSub, setNewSub] = React.useState("");
  const [basedOn, setBasedOn] = React.useState<string>("");
  const [name, setName] = React.useState("");
  const [busy, setBusy] = React.useState(false);
  const [lines, setLines] = React.useState<{ text: string; miss?: boolean }[]>([]);

  const kindsOfCat = world.kinds.filter((k) => k.ready && k.category === category);
  const subsOfCat = React.useMemo(() => {
    const m = new Map<string, string>(); // label -> kind
    for (const s of world.specs) {
      if (s.subcategory && s.category === category) m.set(s.subcategory, s.kind);
    }
    return [...m.entries()].sort();
  }, [world.specs, category]);

  // resolve step 3 into (kind, subcategory, wants_new_generator)
  const resolved = React.useMemo(() => {
    if (pick === "new") {
      return basedOn
        ? { kind: basedOn, subcategory: newSub.trim() || undefined, isNew: true }
        : null;
    }
    if (pick.startsWith("sub:")) {
      const [, label, k] = pick.split(":");
      return { kind: k, subcategory: label, isNew: false };
    }
    return pick ? { kind: pick, subcategory: undefined, isNew: false } : null;
  }, [pick, basedOn, newSub]);

  const ready = desc.trim().length > 0 && resolved != null && name.trim().length > 0
    && (pick !== "new" || newSub.trim().length > 0);

  const go = async () => {
    if (!ready || !resolved) return;
    setBusy(true);
    setLines([]);
    try {
      const r = await forgeApi.createLlm({
        request: desc.trim(),
        kind: resolved.kind,
        name: name.trim(),
        subcategory: resolved.subcategory,
        new_subcategory: resolved.isNew,
      });
      const out: { text: string; miss?: boolean }[] = [];
      if (r.error) {
        out.push({ text: "Claude lane failed: " + r.error, miss: true });
        out.push({ text: "the local grammar answered instead (offline fallback):" });
      } else if (r.source === "llm") {
        out.push({ text: "Claude (" + (r.model ?? "?") + ") translated the description" });
      }
      out.push(...r.understood.map((u) => ({ text: u })));
      for (const w of r.warnings ?? []) out.push({ text: w, miss: true });
      for (const e of r.edits) out.push({ text: e.label + ": " + fmtVal(e.from) + " -> " + fmtVal(e.to) });
      setLines(out);
      if (r.spec && r.kind && r.name) {
        await onCreated(r.kind, r.spec, r.name,
          "New " + r.kind + " species '" + r.name + "' saved as draft"
          + (r.subcategory ? " (sub-category: " + r.subcategory + ")" : "")
          + " — generating seeds");
      }
    } catch (e) {
      setLines([{ text: String(e), miss: true }]);
    } finally {
      setBusy(false);
    }
  };

  const Step = ({ n, label }: { n: number; label: string }) => (
    <div className="flex items-center gap-2 font-display text-xs uppercase tracking-widest text-parch-400">
      <span className="chamfer-sm bevel-up flex h-5 w-5 items-center justify-center bg-gold-600 font-mono text-[11px] text-parch-100">{n}</span>
      {label}
    </div>
  );

  return (
    <Dialog open onOpenChange={(o) => !o && onClose()}>
      <DialogContent className="max-w-2xl">
        <DialogTitle className="flex items-center gap-2">
          <Plus className="h-5 w-5 text-gold-400" /> New asset type
        </DialogTitle>
        <div className="flex flex-col gap-4">
          <div className="flex flex-col gap-1.5">
            <Step n={1} label="Describe what should be generated" />
            <Textarea
              rows={3}
              className="text-xs"
              placeholder='e.g. "a long slender eel with mottled olive skin, ribbon fins, lives in reeds"'
              value={desc}
              onChange={(e) => setDesc(e.target.value)}
            />
          </div>

          <div className="flex flex-col gap-1.5">
            <Step n={2} label="Assign a taxonomy" />
            <Select value={category} onValueChange={(c) => { setCategory(c); setPick(""); setBasedOn(""); }}>
              <SelectTrigger><SelectValue /></SelectTrigger>
              <SelectContent>
                {CATEGORIES.map((c) => (
                  <SelectItem key={c} value={c}>{CATEGORY_LABEL[c] ?? c}</SelectItem>
                ))}
              </SelectContent>
            </Select>
          </div>

          <div className="flex flex-col gap-1.5">
            <Step n={3} label="Sub-category (or create one)" />
            <Select value={pick || undefined} onValueChange={setPick}>
              <SelectTrigger><SelectValue placeholder="pick a sub-category…" /></SelectTrigger>
              <SelectContent>
                <SelectGroup>
                  <SelectLabel>Existing kinds (generators)</SelectLabel>
                  {kindsOfCat.map((k) => (
                    <SelectItem key={k.key} value={k.key}>{k.label}</SelectItem>
                  ))}
                </SelectGroup>
                {subsOfCat.length > 0 && (
                  <SelectGroup>
                    <SelectLabel>Groupings</SelectLabel>
                    {subsOfCat.map(([label, k]) => (
                      <SelectItem key={label} value={"sub:" + label + ":" + k}>
                        {label} (over {k})
                      </SelectItem>
                    ))}
                  </SelectGroup>
                )}
                <SelectItem value="new">+ Create a new sub-category…</SelectItem>
              </SelectContent>
            </Select>
            {pick === "new" && (
              <div className="flex flex-col gap-1.5 pl-7">
                <div className="flex gap-2">
                  <Input
                    className="h-8 text-xs"
                    placeholder="new sub-category name (e.g. eels)"
                    value={newSub}
                    onChange={(e) => setNewSub(e.target.value)}
                  />
                  <Select value={basedOn || undefined} onValueChange={setBasedOn}>
                    <SelectTrigger className="w-44"><SelectValue placeholder="based on…" /></SelectTrigger>
                    <SelectContent>
                      {kindsOfCat.map((k) => (
                        <SelectItem key={k.key} value={k.key}>based on: {k.label}</SelectItem>
                      ))}
                    </SelectContent>
                  </Select>
                </div>
                <p className="text-[11px] text-parch-500">
                  A sub-category is a grouping label. Its geometry comes from the generator it is based on
                  — a truly new generator is code, and this wish is recorded as a TODO in the spec's notes,
                  not pretended.
                </p>
              </div>
            )}
          </div>

          <div className="flex flex-col gap-1.5">
            <Step n={4} label="Name the species" />
            <Input
              className="h-8 font-mono text-xs"
              placeholder="species name (e.g. reed-eel)"
              value={name}
              onChange={(e) => setName(e.target.value)}
            />
          </div>

          <div className="flex items-center gap-2">
            <Button variant="gold" disabled={!ready || busy} onClick={() => void go()}>
              <Sparkles className="h-4 w-4" /> {busy ? "Creating…" : "Create with Claude"}
            </Button>
            <span className="font-mono text-[10px] text-parch-500">
              sends the description to Claude on your subscription · local grammar if offline
            </span>
          </div>

          {lines.length > 0 && (
            <div className="max-h-32 overflow-y-auto font-mono text-[11px]">
              {lines.map((l, i) => (
                <div key={i} className={l.miss ? "text-rust-400" : "text-parch-400"}>{l.text}</div>
              ))}
            </div>
          )}
        </div>
      </DialogContent>
    </Dialog>
  );
}

/* --- plain-language edits ---------------------------------------------------
 * Two lanes, one box. The wand button is forge/language.py -- LOCAL, and the
 * label says so. The Sparkles button is the ONE thing in this app that sends
 * text off the machine: /api/interpret-llm -> the claude CLI on the owner's
 * subscription (owner ruling 2026-09-05). Opt-in per press, never a default,
 * and a failed Claude call reports itself and answers with the local lane. */

function AskPanel({
  spec, onApplied,
}: {
  spec: Spec | null;
  onApplied: (next: Spec, summary: string) => void;
}) {
  const [text, setText] = React.useState("");
  const [busy, setBusy] = React.useState(false);
  const [lines, setLines] = React.useState<{ text: string; miss?: boolean }[]>([]);
  const [conceptCount, setConceptCount] = React.useState<number | null>(null);

  React.useEffect(() => {
    forgeApi.vocabulary().then((v) => setConceptCount(v.concepts.length)).catch(() => {});
  }, []);

  const go = async (viaClaude: boolean) => {
    if (!spec || !text.trim()) return;
    setBusy(true);
    setLines([]);
    try {
      const r = viaClaude
        ? await forgeApi.interpretLlm(spec, text.trim())
        : await forgeApi.interpret(spec, text.trim());
      const out: { text: string; miss?: boolean }[] = [];
      if (viaClaude && r.error) {
        out.push({ text: "Claude lane failed: " + r.error, miss: true });
        out.push({ text: "the local grammar answered instead:" });
      } else if (viaClaude && r.source === "llm") {
        out.push({ text: "Claude (" + (r.model ?? "?") + ") answered; request recorded in the spec's notes" });
      }
      out.push(...r.understood.map((u) => ({ text: u })));
      for (const w of r.warnings ?? []) out.push({ text: w, miss: true });
      if (r.ignored.length) out.push({ text: "didn't understand: " + r.ignored.join(", "), miss: true });
      for (const e of r.edits) out.push({ text: e.label + ": " + fmtVal(e.from) + " -> " + fmtVal(e.to) });
      setLines(out);
      if (r.edits.length) {
        onApplied(r.spec, r.edits.length + " parameter" + (r.edits.length === 1 ? "" : "s") + " changed");
      }
    } catch (e) {
      setLines([{ text: String(e), miss: true }]);
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="mortar-b border-t border-stone-950 bg-stone-800 p-2.5">
      <div className="mb-1 flex items-center gap-1.5 font-display text-xs uppercase tracking-widest text-parch-400">
        <MessageSquareText className="h-3.5 w-3.5" /> Plain speech
        {conceptCount != null && (
          <span className="ml-auto font-mono text-[10px] normal-case tracking-normal text-parch-500">
            {conceptCount} local concepts · wand stays on this machine
          </span>
        )}
      </div>
      <div className="flex gap-2">
        <Textarea
          rows={2}
          className="text-xs"
          placeholder='e.g. "taller, sparser crown, branches to the ground"'
          value={text}
          onChange={(e) => setText(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === "Enter" && !e.shiftKey) {
              e.preventDefault();
              void go(false);
            }
          }}
        />
        <div className="flex flex-col gap-1">
          <Button
            variant="default"
            size="icon"
            disabled={busy || !spec}
            onClick={() => void go(false)}
            title="Apply locally -- nothing leaves this machine"
          >
            <Wand2 className="h-4 w-4" />
          </Button>
          <Button
            variant="ghost"
            size="icon"
            disabled={busy || !spec}
            onClick={() => void go(true)}
            title="Ask Claude -- SENDS this text to Claude on your subscription (claude CLI)"
          >
            <Sparkles className="h-4 w-4" />
          </Button>
        </div>
      </div>
      {lines.length > 0 && (
        <div className="mt-1.5 max-h-24 overflow-y-auto font-mono text-[11px]">
          {lines.map((l, i) => (
            <div key={i} className={l.miss ? "text-rust-400" : "text-parch-400"}>{l.text}</div>
          ))}
        </div>
      )}
    </div>
  );
}

const fmtVal = (v: unknown) =>
  typeof v === "number" ? (Number.isInteger(v) ? String(v) : v.toFixed(2)) : String(v);

/* --- gallery tiles --------------------------------------------------------- */

function SeedTile({
  job, seed, tile, kept, inBank, onOpen, onKeep, onBank,
}: {
  job: string;
  seed: number;
  tile: TileState | undefined;
  kept: boolean;
  inBank: boolean;
  onOpen: () => void;
  onKeep: () => void;
  onBank: () => void;
}) {
  const ready = tile?.ready ?? false;
  const stats = tile?.stats ?? {};
  return (
    <div
      className={cn(
        "chamfer bevel-up group relative flex cursor-pointer flex-col bg-stone-800 transition-colors hover:bg-stone-700",
        kept && "outline outline-2 outline-moss-600",
      )}
      onClick={ready && !tile?.error ? onOpen : undefined}
    >
      <div className="flex aspect-square items-center justify-center overflow-hidden bg-stone-850">
        {!ready ? (
          <span className="flex items-center gap-2 font-display text-xs text-parch-500">
            <RefreshCw className="h-3.5 w-3.5 animate-spin" /> forging…
          </span>
        ) : tile?.error ? (
          <span className="px-2 text-center font-display text-xs text-rust-400">failed</span>
        ) : (
          <img src={forgeApi.tileUrl(job, seed)} alt={"seed " + seed} loading="lazy" className="h-full w-full object-contain" />
        )}
      </div>
      <div className="flex items-center gap-1.5 px-2 py-1.5">
        <span className="font-mono text-xs text-parch-200">seed {seed}</span>
        {ready && !tile?.error && (
          <span className="font-mono text-[10px] text-parch-500">
            {Number(stats["height_m"] ?? 0).toFixed(1)} m · {Number(stats["voxels"] ?? 0).toLocaleString()} vox
          </span>
        )}
        {(tile?.problems ?? []).length > 0 && (
          <span title={(tile!.problems ?? []).join("\n")}>
            <Flag className="h-3.5 w-3.5 text-rust-400" />
          </span>
        )}
      </div>
      {ready && !tile?.error && (
        <div className="absolute right-1.5 top-1.5 flex gap-1 opacity-0 transition-opacity group-hover:opacity-100">
          <button
            className={cn(
              "chamfer-sm bevel-up px-1.5 py-0.5 font-display text-[10px] uppercase tracking-wide",
              kept ? "bg-moss-600 text-parch-100" : "bg-stone-600 text-parch-300 hover:bg-moss-600 hover:text-parch-100",
            )}
            onClick={(e) => {
              e.stopPropagation();
              onKeep();
            }}
            title="Keep this variant to the library (stage 3)"
          >
            {kept ? "kept" : "keep"}
          </button>
          <button
            className={cn(
              "chamfer-sm bevel-up px-1.5 py-0.5 font-display text-[10px] uppercase tracking-wide",
              inBank ? "bg-gold-600 text-parch-100" : "bg-stone-600 text-parch-300 hover:bg-gold-600 hover:text-parch-100",
            )}
            onClick={(e) => {
              e.stopPropagation();
              onBank();
            }}
            title={inBank ? "In the published bank -- click to pull it" : "Publish this seed to the bank"}
          >
            bank
          </button>
        </div>
      )}
    </div>
  );
}

/* --- the seed detail dialog: big 3D preview + stats + keep ----------------- */

const BRANCHLESS = new Set(["rock", "grass", "reed", "flower", "fish", "cetacean", "bird", "quadruped",
                            "artifact"]);

function SeedDetail({
  world, job, seed, species, tile, kept, onKeep, onPlace, onClose,
}: {
  world: World;
  job: string;
  seed: number;
  species: string;
  tile: TileState | undefined;
  kept: boolean;
  onKeep: () => void;
  onPlace?: () => void;
  onClose: () => void;
}) {
  const s = tile?.stats ?? {};
  const kind = String(s["kind"] ?? "");
  const branchy = !BRANCHLESS.has(kind);
  const Icon = kindIcon(kind);
  const num = (k: string) => Number(s[k] ?? 0);
  const rows: [string, string][] = [
    ["voxel size", (s["voxel_cm"] ?? 10) + " cm (preview)"],
    ["height", num("height_m").toFixed(1) + " m"],
    ["solid voxels", num("voxels").toLocaleString()],
    ...(branchy
      ? ([
          ["skeleton nodes", num("nodes").toLocaleString()],
          ["max branch order", String(s["max_order"] ?? "--")],
          ["foliage clumps", num("clumps").toLocaleString()],
          ["wood connected", (Number(s["wood_connected"] ?? 1) * 100).toFixed(2) + "%"],
        ] as [string, string][])
      : []),
    ["ground contact", String(s["ground_contact"] ?? "--") + " voxels"],
    ["build time", String(s["ms_total"] ?? "--") + " ms"],
  ];

  return (
    /* THE VARIANT PANEL (owner directive 2026-09-05): floating, draggable by
     * its title bar, resizable at the corner, twice the old dialog's size by
     * default, and it remembers where you put it. Floating rather than a
     * docked splitter because this view was already an overlay -- it opens
     * over the gallery and closes back into it. The canvas fills the panel,
     * so resizing the panel IS resizing the viewport; orbit/zoom stay bound
     * to the canvas alone (the panel only drags by header and corner). */
    <FloatingPanel
      /* -v2: the owner asked for HALF THE SCREEN as the default; the key
       * bump makes that default beat any earlier persisted box. */
      storageKey="af-panel-seed-detail-v2"
      defaultSize={{
        w: Math.max(700, Math.round(window.innerWidth * 0.5)),
        h: Math.round(window.innerHeight * 0.92),
      }}
      defaultPosition="right"
      onClose={onClose}
      title={
        <span className="flex items-center gap-2">
          <Icon className="h-5 w-5 text-gold-400" />
          {species} · seed {seed}
          {kept && <Badge variant="moss">in library</Badge>}
        </span>
      }
    >
      <div className="grid h-full gap-4 md:grid-cols-[1fr_240px]">
          <VoxelCanvas
            src={forgeApi.jobVoxelsUrl(job, seed)}
            palette={world.palette}
            wrapperClassName="h-full min-h-0"
            className="h-full"
          />
          <div className="overflow-y-auto">
            <table className="w-full border-collapse font-mono text-xs">
              <tbody>
                {rows.map(([k, v]) => (
                  <tr key={k} className="mortar-b">
                    <td className="py-1 pr-2 text-parch-500">{k}</td>
                    <td className="py-1 text-right text-parch-200">{v}</td>
                  </tr>
                ))}
              </tbody>
            </table>
            {(tile?.problems ?? []).map((p) => (
              <div key={p} className="mt-1.5 flex items-start gap-1.5 text-xs text-rust-400">
                <Flag className="mt-0.5 h-3 w-3 shrink-0" /> {p}
              </div>
            ))}
            <div className="mt-3 flex flex-col gap-2">
              <Button variant="moss" size="sm" onClick={onKeep} disabled={kept}>
                <Archive className="h-3.5 w-3.5" /> {kept ? "Already in the library" : "Keep to library"}
              </Button>
              {kept && onPlace && (
                <Button variant="gold" size="sm" onClick={onPlace}>
                  Set placement <ArrowRight className="h-3.5 w-3.5" />
                </Button>
              )}
            </div>
          </div>
      </div>
    </FloatingPanel>
  );
}

import * as React from "react";
import { Check, ChevronLeft, ChevronRight, FileBox, Rotate3d, Stamp, X } from "lucide-react";
import type { World } from "../App";
import { api } from "../lib/api";
import type { CurationStatus, SpeciesRow } from "../lib/schema";
import { CATEGORY_LABEL } from "../lib/schema";
import { CURATION_SEEDS } from "../lib/schema";
import { kindIcon } from "../lib/kindIcons";
import { Badge } from "./ui/badge";
import { Button } from "./ui/button";
import { Input } from "./ui/input";
import { useToast } from "./ui/toast";
import { CurationBadge } from "./LibraryView";
import { LibraryInspector } from "./LibraryInspector";
import { VoxelCanvas } from "./VariantViewer";
import { PlacementPanel } from "./PlacementPanel";
import { cn } from "../lib/cn";

/* One species, everything about it: kept variants (each with a first-class
 * 3D INSPECT affordance), the publish verdict, and the placement panel.
 * Works identically for generated and imported assets -- nothing here
 * assumes generator-only fields. */

export function SpeciesPanel({
  row, world, onVary,
}: {
  row: SpeciesRow;
  world: World;
  onVary?: (spec: Record<string, unknown>, seedStart: number) => void;
}) {
  const Icon = kindIcon(row.kind);
  const variants = world.library.filter((e) => e.species === row.name);
  const [inspecting, setInspecting] = React.useState<string | null>(null);
  const toast = useToast();

  const kindLabel = world.kinds.find((k) => k.key === row.kind)?.label ?? row.kind;
  /* WHAT it is, beside WHICH GENERATOR drew it. Resolved server-side; `null`
   * means nothing can classify it, and that is worth reading as an alarm
   * rather than as a blank -- such a species appears in no index anywhere. */
  const categoryLabel = row.category
    ? (CATEGORY_LABEL[row.category] ?? row.category)
      + (row.category_via === "spec" ? " (per spec)" : "")
    : "NO CATEGORY";

  return (
    <div className="flex flex-col gap-4 p-4">
      <header className="flex items-center gap-3">
        <Icon className="h-7 w-7 text-gold-400" />
        <div className="min-w-0">
          <h2 className="truncate font-display text-2xl tracking-wide text-parch-100">{row.name}</h2>
          <div className="font-mono text-xs text-parch-500">
            <span className={row.category ? "text-parch-400" : "text-rust-400"}>
              {categoryLabel}
            </span>
            {" · "}{kindLabel} · {row.size_m.toFixed(1)} m · {row.shape} · authored at {row.resolution_cm} cm · spec {row.hash}
          </div>
        </div>
        <div className="ml-auto">
          <CurationBadge row={row} />
        </div>
      </header>

      <JudgmentViewport row={row} world={world} />

      <CurationBar row={row} world={world} />

      {/* kept variants: each card IS the inspector's door */}
      <section className="chamfer bevel-up bg-stone-800 p-3">
        <h3 className="mb-2 flex items-center gap-2 font-display text-sm uppercase tracking-widest text-parch-400">
          <FileBox className="h-4 w-4" /> Kept variants
          <span className="font-mono text-[11px] normal-case tracking-normal text-parch-500">
            {variants.length > 0 && variants.length + " saved · click one to inspect it in 3D"}
          </span>
        </h3>
        {variants.length === 0 ? (
          <p className="text-sm text-parch-500">
            None kept yet. Variants arrive from the Forge ("keep to library") or through Import asset --
            both land here and are placement-specced identically.
          </p>
        ) : (
          <div className="grid grid-cols-[repeat(auto-fill,minmax(150px,1fr))] gap-2">
            {variants.map((e) => (
              <button
                key={e.id}
                onClick={() => setInspecting(e.id)}
                className="chamfer bevel-up group relative flex flex-col bg-stone-850 p-1.5 text-left transition-colors hover:bg-stone-700"
                title={"Inspect " + e.id + " in 3D"}
              >
                <img src={api.thumbUrl(e.id)} alt={e.id} loading="lazy" className="h-28 w-full object-contain" />
                <div className="mt-1 flex items-center gap-1.5 px-0.5">
                  <span className="font-mono text-[11px] text-parch-300">
                    {e.imported ? "imported" : "seed " + e.seed}
                  </span>
                  {row.curation.seeds.includes(e.seed) && !e.imported && (
                    <Badge variant="outline" title="In the published bank">bank</Badge>
                  )}
                  <span className="ml-auto flex items-center gap-1 font-display text-[10px] uppercase tracking-wide text-gold-400 opacity-70 transition-opacity group-hover:opacity-100">
                    <Rotate3d className="h-3.5 w-3.5" /> Inspect
                  </span>
                </div>
              </button>
            ))}
          </div>
        )}
      </section>

      <PlacementPanel row={row} world={world} />

      {inspecting && (
        <LibraryInspector
          world={world}
          row={row}
          variants={variants}
          openId={inspecting}
          onOpenChange={setInspecting}
          onVary={onVary}
          onDeleted={async (id) => {
            const rest = variants.filter((e) => e.id !== id);
            setInspecting(rest[0]?.id ?? null);
            await world.refreshLibrary();
            toast.ok("Library updated");
          }}
        />
      )}
    </div>
  );
}

/* --- the judgment viewport (owner directive, 2026-09-05) -----------------
 *
 * "Visually this will be the viewport used to make judgements." The 3D orbit
 * view of each seed variant sits DIRECTLY ABOVE the verdict buttons, so the
 * review loop is look -> orbit -> verdict -> next -- with no kept entry
 * required: the address is (species, seed), regenerated deterministically
 * server-side, which is what lets the 828-species never-reviewed queue be
 * judged at all. Same viewer as the Forge detail and the library inspector
 * (one instanced WebGL2 draw, lib/viewer.ts); switching seeds keeps the
 * camera, so variants are compared from one viewpoint. The seed chips here
 * SWITCH THE VIEW only -- the bank toggles in the verdict bar below keep
 * their own meaning, untouched. */

function JudgmentViewport({ row, world }: { row: SpeciesRow; world: World }) {
  const seeds = React.useMemo(
    () => [...new Set([...CURATION_SEEDS, ...row.curation.seeds])].sort((a, b) => a - b),
    [row.curation.seeds],
  );
  const [seed, setSeed] = React.useState<number>(row.curation.seeds[0] ?? seeds[0] ?? 1);
  const idx = seeds.indexOf(seed);
  const step = (d: number) => setSeed(seeds[(idx + d + seeds.length) % seeds.length] ?? seed);

  return (
    <section className="chamfer bevel-up bg-stone-800 p-3">
      <h3 className="mb-2 flex items-center gap-2 font-display text-sm uppercase tracking-widest text-parch-400">
        <Rotate3d className="h-4 w-4" /> Judgment viewport
        <span className="font-mono text-[11px] normal-case tracking-normal text-parch-500">
          seed {seed}
          {row.curation.seeds.includes(seed) ? " · in the published bank" : " · not in the bank"}
        </span>
      </h3>
      <VoxelCanvas
        src={api.voxelsBySeedUrl(row.name, seed, row.hash)}
        palette={world.palette}
        className="h-96"
      />
      <div className="mt-2 flex items-center gap-1.5">
        <Button variant="ghost" size="icon" className="h-7 w-7" title="Previous seed" onClick={() => step(-1)}>
          <ChevronLeft className="h-4 w-4" />
        </Button>
        <div className="flex flex-1 flex-wrap gap-1.5">
          {seeds.map((s) => (
            <button
              key={s}
              onClick={() => setSeed(s)}
              className={cn(
                "chamfer-sm px-2.5 py-0.5 font-mono text-xs transition-colors",
                s === seed
                  ? "bevel-up bg-gold-600 text-parch-100"
                  : "bevel-down bg-stone-850 text-parch-400 hover:text-parch-200",
              )}
              title={"View seed " + s + " (camera stays put)"}
            >
              {s}
            </button>
          ))}
        </div>
        <Button variant="ghost" size="icon" className="h-7 w-7" title="Next seed" onClick={() => step(1)}>
          <ChevronRight className="h-4 w-4" />
        </Button>
      </div>
    </section>
  );
}

/* --- curation: the publish verdict, written into the spec file ---------- */

function CurationBar({ row, world }: { row: SpeciesRow; world: World }) {
  const toast = useToast();
  const c = row.curation;
  const [seeds, setSeeds] = React.useState<number[]>(c.seeds);
  const [notes, setNotes] = React.useState(c.notes);
  const [busy, setBusy] = React.useState(false);

  const write = async (status: CurationStatus, nextSeeds = seeds) => {
    if (nextSeeds.length === 0) {
      toast.error("At least one seed must stay on -- an approved species with no seeds is a hole in the world.");
      return;
    }
    setBusy(true);
    try {
      const r = await api.saveCuration(row.name, status, nextSeeds, notes);
      world.patchSpec(row.name, { curation: { ...r.curation, curated: true } });
      toast.ok(row.name + ": " + r.curation.status + ", seeds " + r.curation.seeds.join(", "));
    } catch (e) {
      toast.error("Verdict not written: " + String(e));
    } finally {
      setBusy(false);
    }
  };

  const toggleSeed = (s: number) => {
    const next = seeds.includes(s) ? seeds.filter((x) => x !== s) : [...seeds, s].sort((a, b) => a - b);
    setSeeds(next);
    // Approved species re-write immediately on a seed change, so the file
    // never drifts from what the bar shows.
    if (c.curated && next.length > 0) void write(c.status, next);
  };

  const seedMenu = [...new Set([...CURATION_SEEDS, ...c.seeds])].sort((a, b) => a - b);

  return (
    <section className="chamfer bevel-up bg-stone-800 p-3">
      <h3 className="mb-2 flex items-center gap-2 font-display text-sm uppercase tracking-widest text-parch-400">
        <Stamp className="h-4 w-4" /> Publish verdict
      </h3>
      <div className="flex flex-wrap items-center gap-2">
        <Button variant="moss" size="sm" disabled={busy} onClick={() => write("approved")}>
          <Check className="h-3.5 w-3.5" /> Approve
        </Button>
        <Button variant="rust" size="sm" disabled={busy} onClick={() => write("rejected")}>
          <X className="h-3.5 w-3.5" /> Reject
        </Button>
        <Button variant="ghost" size="sm" disabled={busy} onClick={() => write("draft")}>
          Back to draft
        </Button>

        <span className="ml-3 font-mono text-xs text-parch-500">seeds</span>
        {seedMenu.map((s) => (
          <button
            key={s}
            onClick={() => toggleSeed(s)}
            className={cn(
              "chamfer-sm h-7 w-7 font-mono text-xs transition-colors",
              seeds.includes(s)
                ? "bevel-up bg-gold-600 text-parch-100"
                : "bevel-down bg-stone-850 text-parch-500 hover:text-parch-300",
            )}
            title={"Seed " + s + " in the published bank"}
          >
            {s}
          </button>
        ))}

        <Input
          className="ml-auto h-7 w-56 text-xs"
          placeholder="verdict notes…"
          value={notes}
          onChange={(e) => setNotes(e.target.value)}
          onBlur={() => c.curated && notes !== c.notes && void write(c.status)}
        />
      </div>
      {!c.curated && (
        <p className="mt-2 text-xs text-parch-500">
          Never reviewed: exports under the grandfather clause (approved at seeds 1-4) until a verdict is stamped.
        </p>
      )}
    </section>
  );
}

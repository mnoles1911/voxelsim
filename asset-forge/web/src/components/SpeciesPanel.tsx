import { groupLabel } from "../lib/taxonomy";
import * as React from "react";
import { FileBox, Rotate3d } from "lucide-react";
import type { World } from "../App";
import { api } from "../lib/api";
import type { SpeciesRow } from "../lib/schema";
import { CATEGORY_LABEL } from "../lib/schema";
import { kindIcon } from "../lib/kindIcons";
import { Badge } from "./ui/badge";
import { Button } from "./ui/button";
import { useToast } from "./ui/toast";
import { CurationBadge } from "./LibraryView";
import { LibraryInspector } from "./LibraryInspector";
import { VoxelCanvas } from "./VariantViewer";
import { PlacementPanel } from "./PlacementPanel";

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
          <h2 className="truncate font-display text-2xl tracking-wide text-parch-100">{row.design?.display_name||row.name}</h2>
          <div className="font-mono text-xs text-parch-500">
            <span className={row.category ? "text-parch-400" : "text-rust-400"}>
              {categoryLabel}
            </span>
            {" · "}{row.subcategory ? groupLabel(row.subcategory) : kindLabel} · {row.size_m.toFixed(1)} m · {row.shape} · authored at {row.resolution_cm} cm · spec {row.hash}
          </div>
        </div>
        <div className="ml-auto">
          <CurationBadge row={row} />
        </div>
      </header>

      <section className="border border-stone-700 p-3 text-sm text-parch-400">
        <p>Species = baseline recipe. Seed = reproducible input number. Variant = the generated asset.</p>
        {row.design ? <p className="mt-2 text-gold-400">
          Reference variant: <button className="underline" onClick={()=>variants.some(e=>e.id===row.design!.reference_variant_id)?setInspecting(row.design!.reference_variant_id):onVary?.({name:row.name,kind:row.kind},row.design!.reference_seed)}>{row.name} · seed {row.design.reference_seed}</button>
          {row.design.baseline_current ? ' · current baseline' : ' · baseline changed; reference needs updating'}
          {row.design.habitat && ' · '+row.design.habitat}
        </p> : <p className="mt-2">No authoritative reference variant selected yet.</p>}
        {row.design?.reference_notes && <p className="mt-2">{row.design.reference_notes}</p>}
      </section>

      {variants.some((e) => e.imported) ? (
        <section className="chamfer bevel-up bg-stone-800 p-3">
          <h3 className="mb-2 font-display text-sm uppercase tracking-widest text-parch-400">Imported model</h3>
          <VoxelCanvas src={api.voxelsUrl(variants.find((e) => e.imported)!.id)} palette={world.palette} />
        </section>
      ) : <Button onClick={()=>onVary?.({name:row.name,kind:row.kind},1)}>Open source & generate variants in Forge</Button>}

      {/* kept variants: each card IS the inspector's door */}
      <section className="chamfer bevel-up bg-stone-800 p-3">
        <h3 className="mb-2 flex items-center gap-2 font-display text-sm uppercase tracking-widest text-parch-400">
          <FileBox className="h-4 w-4" /> Saved variants
          <span className="font-mono text-[11px] normal-case tracking-normal text-parch-500">
            {variants.length > 0 && variants.length + " saved · click one to inspect it in 3D"}
          </span>
        </h3>
        {variants.length === 0 ? (
          <p className="text-sm text-parch-500">
            No endorsed variants yet. Generate and endorse them in Forge, or import an asset.
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
                    {e.imported ? "imported" : "seed " + e.seed}{e.inventory_candidate ? " · candidate" : ""}
                    {row.design?.reference_variant_id===e.id ? ' · reference' : ''}
                  </span>
                  {row.curation.seeds.includes(e.seed) && !e.imported && !e.inventory_candidate && (
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

      {/* Owner directive 2026-09-05: placement rules apply to environment
        * and creature assets ONLY -- for vehicles (category craftable) the
        * placement UI is ABSENT, not disabled. Category-driven so a future
        * vehicles member inherits it. Mirrors the engine truth, not taste:
        * ADR-0010 puts entity kinds outside world composition, and
        * manifest.species_record refuses them from species.vxm by name --
        * a placement row on a vehicle could never reach the world anyway
        * (verified: all three vehicles carry zero weights and no blocks,
        * and the manifest refusal fires before any placement field is
        * read). */}
      {row.category !== "craftable" && <PlacementPanel row={row} world={world} />}

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


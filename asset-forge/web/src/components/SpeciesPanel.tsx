import * as React from "react";
import { Archive, ChevronLeft, ChevronRight, FileBox, Rotate3d, Stamp, Trash2, X } from "lucide-react";
import type { World } from "../App";
import { api, forgeApi } from "../lib/api";
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
            {" · "}{kindLabel}{row.subcategory ? " · " + row.subcategory + " (grouping)" : ""} · {row.size_m.toFixed(1)} m · {row.shape} · authored at {row.resolution_cm} cm · spec {row.hash}
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
  const toast = useToast();
  /* THE KEPT SET -- the game set, straight off the library. Keeping and
   * unkeeping here is the review gesture (owner ruling 2026-09-05): the
   * server re-derives the species' verdict from the library on every keep
   * and unkeep, so there is no seed list to maintain by hand. */
  const keptSeeds = React.useMemo(
    () => new Set(world.library.filter((e) => e.species === row.name && !e.imported).map((e) => e.seed)),
    [world.library, row.name],
  );
  const seeds = React.useMemo(
    () => [...new Set([...CURATION_SEEDS, ...row.curation.seeds, ...keptSeeds])].sort((a, b) => a - b),
    [row.curation.seeds, keptSeeds],
  );
  const [seed, setSeed] = React.useState<number>(row.curation.seeds[0] ?? seeds[0] ?? 1);
  const [busy, setBusy] = React.useState(false);
  const idx = seeds.indexOf(seed);
  const step = (d: number) => setSeed(seeds[(idx + d + seeds.length) % seeds.length] ?? seed);
  const kept = keptSeeds.has(seed);

  /* Owner directive 2026-09-05: the variant view defaults to TWICE its old
   * size and the user resizes it. This viewport is DOCKED in the species
   * panel's flow (the verdict bar reads directly under it -- floating it
   * would detach the judgment from its verdict), so "resizable + draggable"
   * here is a splitter: drag the bar under the canvas to set its height,
   * remembered across sessions. Old default was 384 px; now 768. */
  const [viewH, setViewH] = React.useState<number>(() => {
    try {
      const n = Number(localStorage.getItem("af-judgment-viewport-h"));
      if (Number.isFinite(n) && n >= 240) return Math.min(n, 1600);
    } catch { /* blocked storage: default is fine */ }
    return 768;
  });
  const viewHRef = React.useRef(viewH);
  viewHRef.current = viewH;
  const dragH = (e: React.PointerEvent) => {
    e.preventDefault();
    const sy = e.clientY;
    const start = viewHRef.current;
    const move = (ev: PointerEvent) =>
      setViewH(Math.min(1600, Math.max(240, start + ev.clientY - sy)));
    const up = () => {
      window.removeEventListener("pointermove", move);
      window.removeEventListener("pointerup", up);
      try {
        localStorage.setItem("af-judgment-viewport-h", String(viewHRef.current));
      } catch { /* forgettable */ }
    };
    window.addEventListener("pointermove", move);
    window.addEventListener("pointerup", up);
  };

  const toggleKeep = async () => {
    setBusy(true);
    try {
      if (kept) {
        await api.deleteLibrary(row.name + "-" + String(seed).padStart(4, "0"));
        toast.ok("Removed " + row.name + " seed " + seed + " from the game set");
      } else {
        const r = await forgeApi.spec(row.name);
        await forgeApi.keep(r.spec, seed);
        toast.ok("Kept " + row.name + " seed " + seed + " — it is in the game set");
      }
      await Promise.all([world.refreshLibrary(), world.refreshSpecs()]);
    } catch (e) {
      toast.error(String(e));
    } finally {
      setBusy(false);
    }
  };

  return (
    <section className="chamfer bevel-up bg-stone-800 p-3">
      <h3 className="mb-2 flex items-center gap-2 font-display text-sm uppercase tracking-widest text-parch-400">
        <Rotate3d className="h-4 w-4" /> Judgment viewport
        <span className="font-mono text-[11px] normal-case tracking-normal text-parch-500">
          seed {seed}
          {kept ? " · KEPT — in the game" : " · not kept"}
        </span>
      </h3>
      <div style={{ height: viewH }}>
        <VoxelCanvas
          src={api.voxelsBySeedUrl(row.name, seed, row.hash)}
          palette={world.palette}
          wrapperClassName="h-full"
          className="h-full"
        />
      </div>
      {/* the splitter: its own element under the canvas, so it can never
        * steal the orbit drag */}
      <div
        onPointerDown={dragH}
        className="mx-auto mt-1 h-2 w-24 cursor-ns-resize rounded bg-stone-600 hover:bg-stone-500"
        title="Drag to resize the viewport (remembered)"
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
                keptSeeds.has(s) && "ring-1 ring-moss-500",
              )}
              title={"View seed " + s + (keptSeeds.has(s) ? " (kept — in the game)" : "") + " · camera stays put"}
            >
              {s}
              {keptSeeds.has(s) && <span className="ml-1 text-moss-400">●</span>}
            </button>
          ))}
        </div>
        <Button variant="ghost" size="icon" className="h-7 w-7" title="Next seed" onClick={() => step(1)}>
          <ChevronRight className="h-4 w-4" />
        </Button>
        {/* THE one review gesture: keep = this variant ships. */}
        <Button
          variant={kept ? "rust" : "moss"}
          size="sm"
          disabled={busy}
          onClick={() => void toggleKeep()}
          title={kept
            ? "Remove this variant from the library (and from the game set)"
            : "Keep this variant to the library — kept variants ARE the game set"}
        >
          {kept ? <><Trash2 className="h-3.5 w-3.5" /> Remove from game</>
                : <><Archive className="h-3.5 w-3.5" /> Keep seed {seed}</>}
        </Button>
      </div>
    </section>
  );
}

/* --- curation: the publish verdict, written into the spec file ---------- */

/* KEEP-DRIVEN (owner ruling 2026-09-05, supersedes the approve+seed-list
 * surface): keeping a seed in the viewport above IS the approval, and the
 * bank is derived from the kept set -- so this bar carries only what keep
 * cannot say: the species-level REJECT ("none of these belong in the
 * game"), its undo, and the verdict notes. The seed toggles are gone as a
 * human surface; the derived bank is displayed, never edited. */

function CurationBar({ row, world }: { row: SpeciesRow; world: World }) {
  const toast = useToast();
  const c = row.curation;
  const [notes, setNotes] = React.useState(c.notes);
  const [busy, setBusy] = React.useState(false);

  const keptSeeds = React.useMemo(
    () => world.library.filter((e) => e.species === row.name && !e.imported)
      .map((e) => e.seed).sort((a, b) => a - b),
    [world.library, row.name],
  );

  const write = async (status: CurationStatus) => {
    // The empty-bank refusal stands server-side; a verdict write always
    // carries the current derived list (or the grandfather default) so a
    // rejection is recorded WITH the seeds it held back.
    const seeds = keptSeeds.length ? keptSeeds : (c.seeds.length ? c.seeds : [...CURATION_SEEDS]);
    setBusy(true);
    try {
      const r = await api.saveCuration(row.name, status, seeds, notes);
      world.patchSpec(row.name, { curation: { ...r.curation, curated: true } });
      toast.ok(row.name + ": " + r.curation.status);
    } catch (e) {
      toast.error("Verdict not written: " + String(e));
    } finally {
      setBusy(false);
    }
  };

  const bankLine = keptSeeds.length
    ? "bank = kept seeds " + keptSeeds.join(", ") + " (keep-driven)"
    : !c.curated
      ? "grandfathered: exports at seeds " + c.seeds.join(", ") + " until reviewed — keep the good seeds above to convert"
      : c.status === "approved"
        ? "legacy seed list " + c.seeds.join(", ") + " — keeping a seed above converts to keep-driven"
        : c.status === "rejected"
          ? "held out of the game"
          : "draft — nothing kept yet, nothing publishes";

  return (
    <section className="chamfer bevel-up bg-stone-800 p-3">
      <h3 className="mb-2 flex items-center gap-2 font-display text-sm uppercase tracking-widest text-parch-400">
        <Stamp className="h-4 w-4" /> Publish verdict
        <span className="font-mono text-[11px] normal-case tracking-normal text-parch-500">{bankLine}</span>
      </h3>
      <div className="flex flex-wrap items-center gap-2">
        {c.status !== "rejected" ? (
          <Button variant="rust" size="sm" disabled={busy} onClick={() => write("rejected")}
            title="None of these belong in the game — holds the species out of every export">
            <X className="h-3.5 w-3.5" /> Reject species
          </Button>
        ) : (
          <Button variant="ghost" size="sm" disabled={busy} onClick={() => write(keptSeeds.length ? "approved" : "draft")}
            title="Lift the rejection">
            Lift rejection
          </Button>
        )}
        <Input
          className="ml-auto h-7 w-64 text-xs"
          placeholder="verdict notes…"
          value={notes}
          onChange={(e) => setNotes(e.target.value)}
          onBlur={() => c.curated && notes !== c.notes && void write(c.status)}
        />
      </div>
      {!c.curated && (
        <p className="mt-2 text-xs text-parch-500">
          Never reviewed: exports under the grandfather clause until you keep seeds (converts to keep-driven) or reject it.
        </p>
      )}
    </section>
  );
}

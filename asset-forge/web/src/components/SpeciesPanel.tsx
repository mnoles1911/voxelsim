import * as React from "react";
import { Check, Download, FileBox, Sparkles, Stamp, Trash2, X } from "lucide-react";
import type { World } from "../App";
import { api, forgeApi } from "../lib/api";
import type { CurationStatus, SpeciesRow } from "../lib/schema";
import { CURATION_SEEDS } from "../lib/schema";
import { kindIcon } from "../lib/kindIcons";
import { Badge } from "./ui/badge";
import { Button } from "./ui/button";
import { Input } from "./ui/input";
import { useToast } from "./ui/toast";
import { CurationBadge } from "./LibraryView";
import { PlacementPanel } from "./PlacementPanel";
import { VariantViewer } from "./VariantViewer";
import { cn } from "../lib/cn";

/* One species, everything about it: variants (with the 3D viewer), the
 * publish verdict, and the placement panel. Works identically for generated
 * and imported assets -- nothing here assumes generator-only fields. */

export function SpeciesPanel({
  row, world, onVary,
}: {
  row: SpeciesRow;
  world: World;
  onVary?: (spec: Record<string, unknown>, seedStart: number) => void;
}) {
  const Icon = kindIcon(row.kind);
  const variants = world.library.filter((e) => e.species === row.name);
  const [viewing, setViewing] = React.useState<string | null>(variants[0]?.id ?? null);
  const toast = useToast();
  const viewingEntry = variants.find((e) => e.id === viewing);

  const kindLabel = world.kinds.find((k) => k.key === row.kind)?.label ?? row.kind;

  return (
    <div className="flex flex-col gap-4 p-4">
      <header className="flex items-center gap-3">
        <Icon className="h-7 w-7 text-gold-400" />
        <div className="min-w-0">
          <h2 className="truncate font-display text-2xl tracking-wide text-parch-100">{row.name}</h2>
          <div className="font-mono text-xs text-parch-500">
            {kindLabel} · {row.size_m.toFixed(1)} m · {row.shape} · authored at {row.resolution_cm} cm · spec {row.hash}
          </div>
        </div>
        <div className="ml-auto">
          <CurationBadge row={row} />
        </div>
      </header>

      <CurationBar row={row} world={world} />

      {/* variants */}
      <section className="chamfer bevel-up bg-stone-800 p-3">
        <h3 className="mb-2 flex items-center gap-2 font-display text-sm uppercase tracking-widest text-parch-400">
          <FileBox className="h-4 w-4" /> Kept variants
        </h3>
        {variants.length === 0 ? (
          <p className="text-sm text-parch-500">
            None kept yet. Variants arrive from the generator's gallery ("keep to library") or through
            Import asset -- both land here and are placement-specced identically.
          </p>
        ) : (
          <>
            <div className="mb-3 flex flex-wrap gap-2">
              {variants.map((e) => (
                <button
                  key={e.id}
                  onClick={() => setViewing(e.id)}
                  className={cn(
                    "chamfer-sm relative w-24 shrink-0 bg-stone-850 p-1 transition-colors",
                    viewing === e.id ? "bevel-up bg-stone-600" : "bevel-down hover:bg-stone-700",
                  )}
                  title={e.id}
                >
                  <img src={api.thumbUrl(e.id)} alt={e.id} loading="lazy" className="h-20 w-full object-contain" />
                  <span className="block truncate text-center font-mono text-[10px] text-parch-400">
                    seed {e.seed}
                  </span>
                  {e.imported && (
                    <Badge variant="gold" className="absolute left-0 top-0">imported</Badge>
                  )}
                </button>
              ))}
            </div>
            {viewing && (
              <div>
                <VariantViewer entryId={viewing} palette={world.palette} />
                <div className="mt-2 flex items-center gap-2">
                  <a href={api.downloadUrl(viewing, "vox")} className="contents">
                    <Button variant="ghost" size="sm"><Download className="h-3.5 w-3.5" /> .vox</Button>
                  </a>
                  <a href={api.downloadUrl(viewing, "vxa")} className="contents">
                    <Button variant="ghost" size="sm"><Download className="h-3.5 w-3.5" /> .vxa</Button>
                  </a>
                  <a href={api.downloadUrl(viewing, "spec")} className="contents">
                    <Button variant="ghost" size="sm"><Download className="h-3.5 w-3.5" /> spec</Button>
                  </a>
                  {onVary && !viewingEntry?.imported && (
                    <Button
                      variant="default"
                      size="sm"
                      title="Back to the Forge: generate fresh seeds from this entry's exact spec"
                      onClick={async () => {
                        try {
                          const r = await forgeApi.librarySpec(viewing);
                          onVary(r.spec, Math.floor(Math.random() * 90000) + 1);
                        } catch (e) {
                          toast.error(String(e));
                        }
                      }}
                    >
                      <Sparkles className="h-3.5 w-3.5" /> More like this
                    </Button>
                  )}
                  <Button
                    variant="rust"
                    size="sm"
                    className="ml-auto"
                    onClick={async () => {
                      if (!confirm("Remove " + viewing + " from the library? This deletes its files on disk.")) return;
                      try {
                        await api.deleteLibrary(viewing);
                        setViewing(variants.find((e) => e.id !== viewing)?.id ?? null);
                        await world.refreshLibrary();
                        toast.ok("Removed " + viewing);
                      } catch (e) {
                        toast.error(String(e));
                      }
                    }}
                  >
                    <Trash2 className="h-3.5 w-3.5" /> Remove
                  </Button>
                </div>
              </div>
            )}
          </>
        )}
      </section>

      <PlacementPanel row={row} world={world} />
    </div>
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

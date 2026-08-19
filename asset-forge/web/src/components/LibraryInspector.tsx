import * as React from "react";
import {
  ChevronLeft, ChevronRight, Download, Landmark, Map as MapIcon, Sparkles, Trash2,
} from "lucide-react";
import type { World } from "../App";
import { api, forgeApi } from "../lib/api";
import type { LibraryEntry, SpeciesRow } from "../lib/schema";
import { allowedBiomes } from "../lib/schema";
import { kindIcon } from "../lib/kindIcons";
import { Badge } from "./ui/badge";
import { Button } from "./ui/button";
import { Dialog, DialogContent, DialogTitle } from "./ui/dialog";
import { useToast } from "./ui/toast";
import { CurationBadge } from "./LibraryView";
import { VoxelCanvas, type DecodedInfo } from "./VariantViewer";
import { cn } from "../lib/cn";

/* THE LIBRARY INSPECTOR: first-class 3D inspection of a SAVED asset.
 *
 * A big orbiting canvas with visible controls, seed switching that keeps the
 * camera (compare a species' kept seeds from one viewpoint), and the numbers
 * that judge a kept asset: voxel count, bounding box in metres, voxel pitch,
 * height, materials used (tallied client-side from the decoded lattice), seed,
 * spec hash, the curation verdict, and -- tying the app's two halves together
 * -- exactly which biomes and named rules the species places under. */

export function LibraryInspector({
  world, row, variants, openId, onOpenChange, onVary, onDeleted,
}: {
  world: World;
  row: SpeciesRow;
  variants: LibraryEntry[];
  openId: string;
  onOpenChange: (id: string | null) => void;
  onVary?: (spec: Record<string, unknown>, seedStart: number) => void;
  onDeleted: (id: string) => void;
}) {
  const toast = useToast();
  const entry = variants.find((e) => e.id === openId) ?? variants[0];
  const idx = variants.findIndex((e) => e.id === entry?.id);
  const [decoded, setDecoded] = React.useState<DecodedInfo | null>(null);
  const Icon = kindIcon(row.kind);

  if (!entry) return null;

  const step = (d: number) => {
    const next = variants[(idx + d + variants.length) % variants.length];
    if (next) {
      setDecoded(null);
      onOpenChange(next.id);
    }
  };

  const stats = entry.stats ?? {};
  const cm = decoded?.cm ?? null;
  const bbox =
    decoded && cm != null
      ? decoded.dims.map((d) => ((d * cm) / 100).toFixed(1)).join(" x ") + " m"
      : "…";
  const matRows = decoded
    ? Object.entries(decoded.materialCounts).sort((a, b) => b[1] - a[1])
    : [];
  const allowed = allowedBiomes(row, world.biomes);

  return (
    <Dialog open onOpenChange={(o) => !o && onOpenChange(null)}>
      <DialogContent className="max-w-5xl">
        <DialogTitle className="flex items-center gap-2 pr-8">
          <Icon className="h-5 w-5 text-gold-400" />
          {entry.id}
          {entry.imported && <Badge variant="gold">imported</Badge>}
          <CurationBadge row={row} />
          {row.curation.seeds.includes(entry.seed) && !entry.imported && (
            <Badge variant="outline" title="This seed is in the published bank">in bank</Badge>
          )}
        </DialogTitle>

        <div className="grid gap-4 lg:grid-cols-[1fr_290px]">
          <div>
            <VoxelCanvas
              src={api.voxelsUrl(entry.id)}
              palette={world.palette}
              className="h-[480px]"
              onDecoded={setDecoded}
            />
            {/* seed switching without losing the camera */}
            {variants.length > 1 && (
              <div className="mt-2 flex items-center gap-1.5">
                <Button variant="ghost" size="icon" className="h-7 w-7" title="Previous kept seed" onClick={() => step(-1)}>
                  <ChevronLeft className="h-4 w-4" />
                </Button>
                <div className="flex flex-1 flex-wrap gap-1.5">
                  {variants.map((e) => (
                    <button
                      key={e.id}
                      onClick={() => onOpenChange(e.id)}
                      className={cn(
                        "chamfer-sm px-2 py-0.5 font-mono text-xs transition-colors",
                        e.id === entry.id
                          ? "bevel-up bg-gold-600 text-parch-100"
                          : "bevel-down bg-stone-850 text-parch-400 hover:text-parch-200",
                      )}
                      title={e.id}
                    >
                      {e.imported ? "import" : "seed " + e.seed}
                    </button>
                  ))}
                </div>
                <Button variant="ghost" size="icon" className="h-7 w-7" title="Next kept seed" onClick={() => step(1)}>
                  <ChevronRight className="h-4 w-4" />
                </Button>
              </div>
            )}
          </div>

          <div className="flex min-w-0 flex-col gap-3">
            <table className="w-full border-collapse font-mono text-xs">
              <tbody>
                {(
                  [
                    ["seed", entry.imported ? "imported (no seed)" : String(entry.seed)],
                    ["height", Number(stats["height_m"] ?? 0).toFixed(1) + " m"],
                    ["bounding box", bbox],
                    ["voxel pitch", cm != null ? cm + " cm" + (decoded?.authoredCm && decoded.authoredCm !== cm ? " shown (exports at " + decoded.authoredCm + " cm)" : "") : "…"],
                    ["solid voxels", Number(stats["voxels"] ?? 0).toLocaleString()],
                    ["surface voxels", decoded ? decoded.count.toLocaleString() : "…"],
                    ["spec hash", String(entry.spec_hash ?? row.hash)],
                  ] as [string, string][]
                ).map(([k, v]) => (
                  <tr key={k} className="mortar-b align-top">
                    <td className="py-1 pr-2 text-parch-500">{k}</td>
                    <td className="break-all py-1 text-right text-parch-200">{v}</td>
                  </tr>
                ))}
              </tbody>
            </table>

            {(entry.problems ?? []).length > 0 && (
              <div className="text-xs text-rust-400">{(entry.problems ?? []).join(" · ")}</div>
            )}

            {/* materials used, tallied from the decoded lattice */}
            <div>
              <div className="mb-1 flex items-center gap-1.5 font-display text-xs uppercase tracking-widest text-parch-400">
                <Landmark className="h-3.5 w-3.5" /> Materials used {matRows.length > 0 && "(" + matRows.length + ")"}
              </div>
              <div className="flex flex-wrap gap-1.5">
                {matRows.map(([m, n]) => {
                  const c = world.palette[m] ?? [255, 0, 255];
                  return (
                    <span key={m} className="chamfer-sm bevel-up flex items-center gap-1.5 bg-stone-850 px-1.5 py-0.5 font-mono text-[10px] text-parch-300" title={"material " + m}>
                      <span className="bevel-up inline-block h-3 w-3" style={{ backgroundColor: "rgb(" + c.join(",") + ")" }} />
                      {n.toLocaleString()}
                    </span>
                  );
                })}
                {matRows.length === 0 && <span className="font-mono text-xs text-parch-500">…</span>}
              </div>
            </div>

            {/* the tie to the app's other half: where this asset places */}
            <div>
              <div className="mb-1 flex items-center gap-1.5 font-display text-xs uppercase tracking-widest text-parch-400">
                <MapIcon className="h-3.5 w-3.5" /> Placed under
              </div>
              {allowed.length === 0 ? (
                <div className="text-xs text-parch-500">Allowed nowhere -- set placement below.</div>
              ) : (
                <div className="flex flex-col gap-1">
                  {allowed.map((k) => {
                    const b = world.biomes.find((x) => x.key === k);
                    const rules = row.biome_rules[k] ?? [];
                    return (
                      <div key={k} className="font-mono text-xs text-parch-300">
                        {b?.label ?? k}
                        <span className="text-parch-500"> · weight {(row.weights[k] ?? 0).toFixed(2)}</span>
                        {rules.length > 0 && <span className="text-gold-400"> · {rules.join(", ")}</span>}
                      </div>
                    );
                  })}
                </div>
              )}
              <Button variant="ghost" size="sm" className="mt-1.5" onClick={() => onOpenChange(null)}>
                Edit placement in the panel below
              </Button>
            </div>

            <div className="mt-auto flex flex-col gap-2">
              <div className="flex gap-2">
                <a href={api.downloadUrl(entry.id, "vox")} className="contents">
                  <Button variant="ghost" size="sm"><Download className="h-3.5 w-3.5" /> .vox</Button>
                </a>
                <a href={api.downloadUrl(entry.id, "vxa")} className="contents">
                  <Button variant="ghost" size="sm"><Download className="h-3.5 w-3.5" /> .vxa</Button>
                </a>
                <a href={api.downloadUrl(entry.id, "spec")} className="contents">
                  <Button variant="ghost" size="sm"><Download className="h-3.5 w-3.5" /> spec</Button>
                </a>
              </div>
              {onVary && !entry.imported && (
                <Button
                  variant="default"
                  size="sm"
                  title="Back to the Forge: generate fresh seeds from this entry's exact spec"
                  onClick={async () => {
                    try {
                      const r = await forgeApi.librarySpec(entry.id);
                      onOpenChange(null);
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
                onClick={async () => {
                  if (!confirm("Remove " + entry.id + " from the library? This deletes its files on disk.")) return;
                  try {
                    await api.deleteLibrary(entry.id);
                    toast.ok("Removed " + entry.id);
                    onDeleted(entry.id);
                  } catch (e) {
                    toast.error(String(e));
                  }
                }}
              >
                <Trash2 className="h-3.5 w-3.5" /> Remove from library
              </Button>
            </div>
          </div>
        </div>
      </DialogContent>
    </Dialog>
  );
}

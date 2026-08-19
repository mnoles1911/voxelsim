import * as React from "react";
import { Search } from "lucide-react";
import type { World } from "../App";
import type { SpeciesRow } from "../lib/schema";
import { allowedBiomes } from "../lib/schema";
import { kindIcon } from "../lib/kindIcons";
import { Badge } from "./ui/badge";
import { Input } from "./ui/input";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "./ui/select";
import { SpeciesPanel } from "./SpeciesPanel";
import { cn } from "../lib/cn";

/* THE HOME: the whole species library in one filterable ledger, with the
 * selected species' variants, curation and placement in a panel alongside.
 * Placement is integrated here, not a separate page (owner directive). */

export function LibraryView({
  world, focus, onVary,
}: {
  world: World;
  /** stage-4 handoff from the Forge or an import: open on this species */
  focus?: { name: string; n: number } | null;
  /** back to stage 1: regenerate variations of a kept entry's exact spec */
  onVary?: (spec: Record<string, unknown>, seedStart: number) => void;
}) {
  const [kind, setKind] = React.useState("all");
  const [biome, setBiome] = React.useState("all");
  // "What is actually in my library" defaults to the exporting set: approved
  // verdicts (the grandfathered majority included). Switch to "Any verdict"
  // to see drafts and rejections.
  const [status, setStatus] = React.useState("approved");
  const [query, setQuery] = React.useState("");
  const [selected, setSelected] = React.useState<string | null>(null);

  React.useEffect(() => {
    if (!focus) return;
    setSelected(focus.name);
    // A handoff (just-kept seed, fresh import at draft) must never land on a
    // ledger that filters its species out of sight.
    setStatus("all");
  }, [focus]);

  const variantCount = React.useMemo(() => {
    const m = new Map<string, number>();
    for (const e of world.library) m.set(e.species, (m.get(e.species) ?? 0) + 1);
    return m;
  }, [world.library]);

  const rows = React.useMemo(() => {
    const q = query.trim().toLowerCase();
    return world.specs.filter((s) => {
      if (kind !== "all" && s.kind !== kind) return false;
      if (status !== "all") {
        if (status === "unreviewed" ? s.curation.curated : s.curation.status !== status) return false;
      }
      if (biome !== "all") {
        const allowed = allowedBiomes(s, world.biomes);
        if (biome === "unassigned" ? allowed.length > 0 : !allowed.includes(biome)) return false;
      }
      if (q && !s.name.includes(q) && !s.notes.toLowerCase().includes(q)) return false;
      return true;
    });
  }, [world.specs, world.biomes, kind, biome, status, query]);

  const selectedRow = world.specs.find((s) => s.name === selected) ?? null;

  return (
    <div className="flex min-h-0 flex-1">
      {/* ledger side */}
      <div className="flex min-h-0 w-[440px] shrink-0 flex-col border-r-2 border-stone-950">
        <div className="mortar-b flex flex-col gap-2 bg-stone-850 p-3">
          <div className="relative">
            <Search className="absolute left-2.5 top-1/2 h-4 w-4 -translate-y-1/2 text-parch-500" />
            <Input
              className="pl-8"
              placeholder="Search the ledger…"
              value={query}
              onChange={(e) => setQuery(e.target.value)}
            />
          </div>
          <div className="grid grid-cols-3 gap-2">
            <Select value={kind} onValueChange={setKind}>
              <SelectTrigger><SelectValue /></SelectTrigger>
              <SelectContent>
                <SelectItem value="all">All kinds</SelectItem>
                {world.kinds.map((k) => (
                  <SelectItem key={k.key} value={k.key}>{k.label}</SelectItem>
                ))}
              </SelectContent>
            </Select>
            <Select value={biome} onValueChange={setBiome}>
              <SelectTrigger><SelectValue /></SelectTrigger>
              <SelectContent>
                <SelectItem value="all">All biomes</SelectItem>
                <SelectItem value="unassigned">Unassigned</SelectItem>
                {world.biomes.filter((b) => b.hosts.length > 0).map((b) => (
                  <SelectItem key={b.key} value={b.key}>{b.label}</SelectItem>
                ))}
              </SelectContent>
            </Select>
            <Select value={status} onValueChange={setStatus}>
              <SelectTrigger><SelectValue /></SelectTrigger>
              <SelectContent>
                <SelectItem value="all">Any verdict</SelectItem>
                <SelectItem value="approved">Approved</SelectItem>
                <SelectItem value="draft">Draft</SelectItem>
                <SelectItem value="rejected">Rejected</SelectItem>
                <SelectItem value="unreviewed">Never reviewed</SelectItem>
              </SelectContent>
            </Select>
          </div>
          <div className="font-mono text-xs text-parch-500">{rows.length} species shown</div>
        </div>

        <div className="min-h-0 flex-1 overflow-y-auto">
          {rows.map((s) => (
            <SpeciesLine
              key={s.name}
              row={s}
              world={world}
              variants={variantCount.get(s.name) ?? 0}
              active={s.name === selected}
              onClick={() => setSelected(s.name)}
            />
          ))}
          {rows.length === 0 && (
            <div className="p-6 text-center font-display text-sm text-parch-500">
              Nothing in the ledger matches.
            </div>
          )}
        </div>
      </div>

      {/* detail side */}
      <div className="min-h-0 min-w-0 flex-1 overflow-y-auto">
        {selectedRow ? (
          <SpeciesPanel key={selectedRow.name} row={selectedRow} world={world} onVary={onVary} />
        ) : (
          <div className="flex h-full items-center justify-center p-8 text-center font-display text-parch-500">
            Choose a species from the ledger to see its variants,
            <br />
            verdict and placement.
          </div>
        )}
      </div>
    </div>
  );
}

function SpeciesLine({
  row, world, variants, active, onClick,
}: {
  row: SpeciesRow;
  world: World;
  variants: number;
  active: boolean;
  onClick: () => void;
}) {
  const Icon = kindIcon(row.kind);
  const allowed = allowedBiomes(row, world.biomes);
  return (
    <button
      onClick={onClick}
      className={cn(
        "mortar-b flex w-full items-center gap-3 px-3 py-2 text-left transition-colors",
        active ? "bg-stone-700" : "hover:bg-stone-800",
      )}
    >
      <Icon className={cn("h-4 w-4 shrink-0", active ? "text-gold-400" : "text-parch-500")} />
      <div className="min-w-0 flex-1">
        <div className="truncate text-sm text-parch-100">{row.name}</div>
        <div className="truncate font-mono text-[11px] text-parch-500">
          {row.size_m.toFixed(1)} m · {allowed.length === 0 ? "nowhere" : allowed.length + " biome" + (allowed.length > 1 ? "s" : "")}
          {variants > 0 && " · " + variants + " kept"}
          {Object.keys(row.biome_rules).length > 0 && " · ruled"}
        </div>
      </div>
      <CurationBadge row={row} />
    </button>
  );
}

export function CurationBadge({ row }: { row: SpeciesRow }) {
  const c = row.curation;
  if (!c.curated) return <Badge variant="outline">unreviewed</Badge>;
  if (c.status === "approved") return <Badge variant="moss">approved</Badge>;
  if (c.status === "rejected") return <Badge variant="rust">rejected</Badge>;
  return <Badge>draft</Badge>;
}

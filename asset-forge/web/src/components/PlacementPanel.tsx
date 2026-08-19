import * as React from "react";
import { CircleAlert, Map as MapIcon, Plus, Save, ScrollText, Undo2, X } from "lucide-react";
import type { World } from "../App";
import { api } from "../lib/api";
import type { AllowlistMode, SpeciesRow } from "../lib/schema";
import { allowlistMode, composeRules, describeRule } from "../lib/schema";
import { Badge } from "./ui/badge";
import { Button } from "./ui/button";
import { Checkbox } from "./ui/checkbox";
import { Input } from "./ui/input";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "./ui/select";
import { useToast } from "./ui/toast";
import { cn } from "../lib/cn";

/* THE PLACEMENT PANEL (owner directive, verbatim order):
 *   1. the asset is assigned to one or many biomes  -> weight checkboxes
 *   2. allowlist semantics: one, many, or none      -> the three-mode select
 *   3. one or many named rule overrides per biome   -> attach dropdowns
 * plus the kind x biome density as READ-ONLY context, since it scales
 * everything this panel authors.
 *
 * Writes go to /api/placement, which edits only the touched blocks of the raw
 * spec file -- the diff is exactly what was changed here. */

export function PlacementPanel({ row, world }: { row: SpeciesRow; world: World }) {
  const toast = useToast();
  const hosted = world.biomes.filter((b) => b.hosts.includes(row.kind));
  const density = world.rules.density[row.kind] ?? {};
  const ruleNames = Object.keys(world.rules.rules).sort();

  const [weights, setWeights] = React.useState<Record<string, number>>({ ...row.weights });
  const [mode, setMode] = React.useState<AllowlistMode>(allowlistMode(row.biome_allow));
  const [allowSet, setAllowSet] = React.useState<string[]>(row.biome_allow ?? []);
  const [rules, setRules] = React.useState<Record<string, string[]>>(
    Object.fromEntries(Object.entries(row.biome_rules).map(([k, v]) => [k, [...v]])),
  );
  const [busy, setBusy] = React.useState(false);

  /* what the save would write, block by block; empty object = nothing dirty */
  const payload = React.useMemo(() => {
    const p: {
      weights?: Record<string, number>;
      biome_allow?: string[] | null;
      biome_rules?: Record<string, string[]> | null;
    } = {};
    const wDelta: Record<string, number> = {};
    for (const b of hosted) {
      const before = row.weights[b.key] ?? 0;
      const after = weights[b.key] ?? 0;
      if (before !== after) wDelta[b.key] = after;
    }
    if (Object.keys(wDelta).length) p.weights = wDelta;

    const allowAfter = mode === "derived" ? null : mode === "nowhere" ? [] : [...allowSet].sort();
    const allowBefore = row.biome_allow == null ? null : [...row.biome_allow].sort();
    if (JSON.stringify(allowAfter) !== JSON.stringify(allowBefore)) p.biome_allow = allowAfter;

    const rulesAfter: Record<string, string[]> = {};
    for (const [k, v] of Object.entries(rules)) if (v.length) rulesAfter[k] = [...v].sort();
    const rulesBefore: Record<string, string[]> = {};
    for (const [k, v] of Object.entries(row.biome_rules)) if (v.length) rulesBefore[k] = [...v].sort();
    if (JSON.stringify(rulesAfter) !== JSON.stringify(rulesBefore))
      p.biome_rules = Object.keys(rulesAfter).length ? rulesAfter : null;
    return p;
  }, [hosted, weights, mode, allowSet, rules, row]);

  const dirty = Object.keys(payload).length > 0;

  /* the resolver's answer under the current draft (preview of forge.biomes.allowed) */
  const effectiveAllowed =
    mode === "nowhere"
      ? []
      : mode === "explicit"
        ? allowSet
        : hosted.filter((b) => (weights[b.key] ?? 0) > 0).map((b) => b.key);

  const save = async () => {
    setBusy(true);
    try {
      const r = await api.savePlacement(row.name, payload);
      world.patchSpec(row.name, {
        weights: r.weights,
        biome_allow: r.biome_allow,
        biome_rules: r.biome_rules,
      });
      setWeights({ ...r.weights });
      setMode(allowlistMode(r.biome_allow));
      setAllowSet(r.biome_allow ?? []);
      setRules(Object.fromEntries(Object.entries(r.biome_rules).map(([k, v]) => [k, [...v]])));
      for (const w of r.warnings) toast.error(w);
      toast.ok(row.name + ": placement written (allowed in " + (r.allowed.length || "no") + " biome" + (r.allowed.length === 1 ? "" : "s") + ")");
    } catch (e) {
      toast.error("Placement not written: " + String(e));
    } finally {
      setBusy(false);
    }
  };

  const revert = () => {
    setWeights({ ...row.weights });
    setMode(allowlistMode(row.biome_allow));
    setAllowSet(row.biome_allow ?? []);
    setRules(Object.fromEntries(Object.entries(row.biome_rules).map(([k, v]) => [k, [...v]])));
  };

  return (
    <section className="chamfer bevel-up bg-stone-800 p-3">
      <div className="mb-3 flex items-center gap-2">
        <h3 className="flex items-center gap-2 font-display text-sm uppercase tracking-widest text-parch-400">
          <MapIcon className="h-4 w-4" /> Placement
        </h3>
        <div className="ml-auto flex items-center gap-2">
          {dirty && (
            <Button variant="ghost" size="sm" onClick={revert} disabled={busy}>
              <Undo2 className="h-3.5 w-3.5" /> Revert
            </Button>
          )}
          <Button variant="gold" size="sm" onClick={save} disabled={!dirty || busy}>
            <Save className="h-3.5 w-3.5" /> Save placement
          </Button>
        </div>
      </div>

      {/* allowlist semantics: one / many / none */}
      <div className="mb-3 flex items-center gap-3">
        <span className="font-display text-sm text-parch-300">Allowlist</span>
        <div className="w-72">
          <Select value={mode} onValueChange={(v) => setMode(v as AllowlistMode)}>
            <SelectTrigger><SelectValue /></SelectTrigger>
            <SelectContent>
              <SelectItem value="derived">Derived from biome weights</SelectItem>
              <SelectItem value="explicit">Explicit list (fence it in)</SelectItem>
              <SelectItem value="nowhere">Allowed nowhere</SelectItem>
            </SelectContent>
          </Select>
        </div>
        <span className="font-mono text-xs text-parch-500">
          {mode === "derived" && "allowed exactly where its weights are positive"}
          {mode === "explicit" && "allowed ONLY in the checked biomes; weights elsewhere are zeroed at export"}
          {mode === "nowhere" && "this species places in no biome at all"}
        </span>
      </div>

      <div className="overflow-x-auto">
        <table className="w-full border-collapse text-sm">
          <thead>
            <tr className="mortar-b text-left font-display text-xs uppercase tracking-widest text-parch-500">
              <th className="px-2 py-1.5">Biome</th>
              <th className="px-2 py-1.5" title="Species pick weight, 0..1">Weight</th>
              <th className="px-2 py-1.5" title="rules/biome-density.json -- scales every placement in this biome; edited in its own file, shown here as context">
                Density
              </th>
              {mode === "explicit" && <th className="px-2 py-1.5">Allowed</th>}
              <th className="w-full px-2 py-1.5">Rule overrides</th>
            </tr>
          </thead>
          <tbody>
            {hosted.map((b) => {
              const member = (weights[b.key] ?? 0) > 0;
              const inAllow = effectiveAllowed.includes(b.key);
              const attached = rules[b.key] ?? [];
              const composed = composeRules(attached.map((n) => world.rules.rules[n]).filter(Boolean));
              return (
                <tr key={b.key} className={cn("mortar-b align-top", !inAllow && "opacity-50")}>
                  <td className="whitespace-nowrap px-2 py-2">
                    <label className="flex cursor-pointer items-center gap-2">
                      <Checkbox
                        checked={member}
                        onCheckedChange={(on) =>
                          setWeights((w) => ({ ...w, [b.key]: on ? (row.weights[b.key] || 0.5) : 0 }))
                        }
                      />
                      <span className={cn(member ? "text-parch-100" : "text-parch-500")}>{b.label}</span>
                    </label>
                  </td>
                  <td className="px-2 py-2">
                    <Input
                      type="number"
                      min={0}
                      max={1}
                      step={0.05}
                      disabled={!member}
                      value={member ? (weights[b.key] ?? 0) : 0}
                      onChange={(e) => {
                        const v = Math.max(0, Math.min(1, Number(e.target.value)));
                        setWeights((w) => ({ ...w, [b.key]: v }));
                      }}
                      className="h-7 w-20 text-xs"
                    />
                  </td>
                  <td className="px-2 py-2">
                    <span
                      className="font-mono text-xs text-parch-400"
                      title="Read-only: the kind x biome density table scales the keep test for every placement here"
                    >
                      {((density[b.key] ?? 1) * 100).toFixed(0)}%
                    </span>
                  </td>
                  {mode === "explicit" && (
                    <td className="px-2 py-2">
                      <Checkbox
                        checked={allowSet.includes(b.key)}
                        onCheckedChange={(on) =>
                          setAllowSet((s) => (on ? [...s, b.key] : s.filter((k) => k !== b.key)))
                        }
                      />
                    </td>
                  )}
                  <td className="px-2 py-2">
                    <div className="flex flex-wrap items-center gap-1.5">
                      {attached.map((n) => (
                        <Badge key={n} variant={world.rules.rules[n] ? "default" : "rust"} title={world.rules.rules[n] ? describeRule(world.rules.rules[n]) : "UNKNOWN RULE: the export will refuse this species by name"}>
                          <ScrollText className="h-3 w-3" />
                          {n}
                          <button
                            className="ml-0.5 hover:text-rust-400"
                            onClick={() =>
                              setRules((r) => ({ ...r, [b.key]: (r[b.key] ?? []).filter((x) => x !== n) }))
                            }
                          >
                            <X className="h-3 w-3" />
                          </button>
                        </Badge>
                      ))}
                      <AttachRule
                        options={ruleNames.filter((n) => !attached.includes(n))}
                        describe={(n) => describeRule(world.rules.rules[n])}
                        onAttach={(n) =>
                          setRules((r) => ({ ...r, [b.key]: [...(r[b.key] ?? []), n] }))
                        }
                      />
                    </div>
                    {attached.length > 1 && (
                      <div className="mt-1 font-mono text-[11px] text-parch-500">
                        composed (strictest wins): {describeRule(composed.effective)}
                      </div>
                    )}
                    {composed.conflicts.map((c) => (
                      <div key={c} className="mt-1 flex items-start gap-1 text-[11px] text-rust-400">
                        <CircleAlert className="mt-0.5 h-3 w-3 shrink-0" /> {c}
                      </div>
                    ))}
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>

      <p className="mt-2 font-mono text-xs text-parch-500">
        Effective allowlist: {effectiveAllowed.length === 0 ? "nowhere" : effectiveAllowed.join(", ")}
        {mode === "explicit" &&
          hosted.some((b) => (weights[b.key] ?? 0) > 0 && !allowSet.includes(b.key)) &&
          " -- weighted biomes outside the list export as zero"}
      </p>
    </section>
  );
}

/** The "attach a named rule" dropdown: a Radix select used as an action menu,
 * so the list always shows every authored rule with its plain-English body. */
function AttachRule({
  options, describe, onAttach,
}: {
  options: string[];
  describe: (name: string) => string;
  onAttach: (name: string) => void;
}) {
  const [key, setKey] = React.useState(0); // remount to clear the value after each attach
  if (options.length === 0) return null;
  return (
    <Select
      key={key}
      onValueChange={(v) => {
        onAttach(v);
        setKey((k) => k + 1);
      }}
    >
      <SelectTrigger className="h-6 w-auto gap-1 bg-stone-700 px-2 text-xs text-parch-400">
        <Plus className="h-3 w-3" />
        <SelectValue placeholder="attach rule" />
      </SelectTrigger>
      <SelectContent className="max-w-md">
        {options.map((n) => (
          <SelectItem key={n} value={n}>
            <span className="font-mono">{n}</span>
            <span className="ml-2 text-xs text-parch-400">{describe(n)}</span>
          </SelectItem>
        ))}
      </SelectContent>
    </Select>
  );
}

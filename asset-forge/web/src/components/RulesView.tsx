import * as React from "react";
import { CircleAlert, Pencil, Plus, ScrollText, Table2, Trash2 } from "lucide-react";
import type { World } from "../App";
import { api } from "../lib/api";
import type { PlacementRule, RuleField, WaterKind } from "../lib/schema";
import {
  RULE_FIELDS, RULE_FIELD_LABELS, RULE_LIMITS, WATER_KINDS, describeRule, validateRule,
} from "../lib/schema";
import { Badge } from "./ui/badge";
import { Button } from "./ui/button";
import { Checkbox } from "./ui/checkbox";
import { Dialog, DialogContent, DialogDescription, DialogTitle } from "./ui/dialog";
import { Input, Textarea } from "./ui/input";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "./ui/select";
import { useToast } from "./ui/toast";

/* RULES AUTHORING: the named, reusable placement-rule library itself.
 * Rules are authored here FIRST, then attached to (species, biome) pairs in
 * the placement panel by reference. Ranges are validated client-side against
 * the schema seam (lib/schema.ts) before any write. */

export function RulesView({ world }: { world: World }) {
  const toast = useToast();
  const [editing, setEditing] = React.useState<string | null | false>(false); // false=closed, null=new
  const names = Object.keys(world.rules.rules).sort();

  const remove = async (name: string) => {
    const refs = world.rules.referenced[name] ?? [];
    if (refs.length > 0) {
      toast.error(
        "Cannot delete " + name + ": referenced by " + refs.length + " species (" +
          refs.slice(0, 4).join(", ") + (refs.length > 4 ? "…" : "") +
          "). Detach it everywhere first -- a dangling name would make the export refuse.",
      );
      return;
    }
    if (!confirm("Delete the rule '" + name + "' from the library?")) return;
    try {
      await api.deleteRule(name);
      await world.refreshRules();
      toast.ok("Deleted " + name);
    } catch (e) {
      toast.error(String(e));
    }
  };

  return (
    <div className="min-h-0 flex-1 overflow-y-auto p-4">
      <div className="mx-auto max-w-5xl">
        <div className="mb-4 flex items-center gap-3">
          <h2 className="flex items-center gap-2 font-display text-xl tracking-wide text-parch-100">
            <ScrollText className="h-5 w-5 text-gold-400" /> The rule library
          </h2>
          <p className="text-sm text-parch-500">
            Authored once, attached to (species, biome) by name. Several rules on one pair compose by
            intersection -- strictest wins.
          </p>
          <Button variant="gold" size="sm" className="ml-auto" onClick={() => setEditing(null)}>
            <Plus className="h-4 w-4" /> New rule
          </Button>
        </div>

        <div className="grid grid-cols-1 gap-3 md:grid-cols-2 xl:grid-cols-3">
          {names.map((name) => {
            const rule = world.rules.rules[name];
            const refs = world.rules.referenced[name] ?? [];
            return (
              <div key={name} className="chamfer bevel-up flex flex-col gap-2 bg-stone-800 p-3">
                <div className="flex items-center gap-2">
                  <span className="font-mono text-sm text-gold-400">{name}</span>
                  <div className="ml-auto flex gap-1">
                    <Button variant="ghost" size="icon" className="h-7 w-7" title="Edit" onClick={() => setEditing(name)}>
                      <Pencil className="h-3.5 w-3.5" />
                    </Button>
                    <Button variant="ghost" size="icon" className="h-7 w-7" title="Delete" onClick={() => void remove(name)}>
                      <Trash2 className="h-3.5 w-3.5" />
                    </Button>
                  </div>
                </div>
                <div className="text-sm text-parch-200">{describeRule(rule)}</div>
                {rule._note && <div className="text-xs italic text-parch-500">{rule._note}</div>}
                <div className="mt-auto pt-1">
                  <Badge variant={refs.length ? "default" : "outline"}>
                    {refs.length ? "used by " + refs.length + " species" : "unused"}
                  </Badge>
                </div>
              </div>
            );
          })}
          {names.length === 0 && (
            <p className="text-sm text-parch-500">No rules authored yet. Rules must exist before they can be attached.</p>
          )}
        </div>

        <DensityTable world={world} />
      </div>

      {editing !== false && (
        <RuleEditor
          world={world}
          name={editing}
          onClose={() => setEditing(false)}
          onSaved={async (n) => {
            await world.refreshRules();
            setEditing(false);
            toast.ok("Rule " + n + " written to the library");
          }}
        />
      )}
    </div>
  );
}

/* --- the kind x biome density table, read-only context ------------------ */

function DensityTable({ world }: { world: World }) {
  const kinds = Object.keys(world.rules.density);
  if (kinds.length === 0) return null;
  const biomeKeys = world.biomes.map((b) => b.key).filter((k) => kinds.some((kd) => world.rules.density[kd][k] !== undefined));
  return (
    <section className="mt-6">
      <h3 className="mb-2 flex items-center gap-2 font-display text-sm uppercase tracking-widest text-parch-400">
        <Table2 className="h-4 w-4" /> Kind x biome density (read-only)
      </h3>
      <p className="mb-2 text-xs text-parch-500">
        Scales every placement of a kind in a biome, after the species pick; it may only thin, never boost.
        Edited in rules/biome-density.json -- a worldgen change with a version bump, not a UI knob.
      </p>
      <div className="chamfer bevel-down overflow-x-auto bg-stone-850 p-2">
        <table className="w-full border-collapse font-mono text-xs">
          <thead>
            <tr>
              <th className="px-2 py-1 text-left text-parch-500">kind</th>
              {biomeKeys.map((k) => (
                <th key={k} className="px-2 py-1 text-right text-parch-500">
                  {world.biomes.find((b) => b.key === k)?.label ?? k}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {kinds.map((kd) => (
              <tr key={kd} className="mortar-b">
                <td className="px-2 py-1 text-parch-200">{kd}</td>
                {biomeKeys.map((bk) => {
                  const v = world.rules.density[kd][bk];
                  return (
                    <td key={bk} className={"px-2 py-1 text-right " + (v !== undefined && v < 1 ? "text-gold-400" : "text-parch-400")}>
                      {v === undefined ? "--" : (v * 100).toFixed(0) + "%"}
                    </td>
                  );
                })}
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </section>
  );
}

/* --- the rule editor dialog --------------------------------------------- */

function RuleEditor({
  world, name, onClose, onSaved,
}: {
  world: World;
  name: string | null; // null = new
  onClose: () => void;
  onSaved: (name: string) => Promise<void>;
}) {
  const existing = name ? world.rules.rules[name] : undefined;
  const [ruleName, setRuleName] = React.useState(name ?? "");
  const [rule, setRule] = React.useState<PlacementRule>(existing ? { ...existing } : {});
  const [busy, setBusy] = React.useState(false);

  const errors = validateRule(ruleName, rule);
  const nameClash = name === null && !!world.rules.rules[ruleName];

  const setField = (f: RuleField, v: number | WaterKind | undefined) =>
    setRule((r) => {
      const next = { ...r };
      if (v === undefined) delete next[f];
      else (next as Record<string, unknown>)[f] = v;
      return next;
    });

  const save = async () => {
    setBusy(true);
    try {
      await api.saveRule(ruleName, rule);
      await onSaved(ruleName);
    } catch (e) {
      setBusy(false);
      alert("Not written: " + String(e));
    }
  };

  return (
    <Dialog open onOpenChange={(o) => !o && onClose()}>
      <DialogContent className="max-w-xl">
        <DialogTitle>{name ? "Edit rule: " + name : "Author a new rule"}</DialogTitle>
        <DialogDescription>
          A rule is a set of placement restrictions with a name. Attach it to any (species, biome) pair
          in the library; every field composes by intersection when several rules share a pair.
          {name && (world.rules.referenced[name]?.length ?? 0) > 0 && (
            <span className="mt-1 block text-gold-400">
              Editing changes the manifest for {world.rules.referenced[name].length} species -- a worldgen
              change (version bump, goldens re-blessed).
            </span>
          )}
        </DialogDescription>

        {name === null && (
          <label className="mb-3 block">
            <span className="mb-1 block font-display text-xs uppercase tracking-widest text-parch-400">Name</span>
            <Input
              value={ruleName}
              onChange={(e) => setRuleName(e.target.value)}
              placeholder="e.g. near-fresh-water-60m"
              className="font-mono"
            />
          </label>
        )}

        <div className="flex flex-col gap-2">
          {RULE_FIELDS.map((f) => {
            const on = rule[f] !== undefined;
            return (
              <div key={f} className="flex items-center gap-3">
                <label className="flex w-56 shrink-0 cursor-pointer items-center gap-2 text-sm text-parch-200">
                  <Checkbox
                    checked={on}
                    onCheckedChange={(v) => {
                      if (!v) return setField(f, undefined);
                      if (f === "water") return setField(f, "any");
                      const lim = RULE_LIMITS[f];
                      setField(f, Math.max(lim.min, Math.min(lim.max, f.includes("max") ? lim.max : lim.min)));
                    }}
                  />
                  {RULE_FIELD_LABELS[f]}
                </label>
                {on &&
                  (f === "water" ? (
                    <div className="w-40">
                      <Select value={rule.water} onValueChange={(v) => setField("water", v as WaterKind)}>
                        <SelectTrigger className="h-7 text-xs"><SelectValue /></SelectTrigger>
                        <SelectContent>
                          {WATER_KINDS.map((w) => (
                            <SelectItem key={w} value={w}>{w}</SelectItem>
                          ))}
                        </SelectContent>
                      </Select>
                    </div>
                  ) : (
                    <>
                      <Input
                        type="number"
                        className="h-7 w-28 text-xs"
                        min={RULE_LIMITS[f].min}
                        max={RULE_LIMITS[f].max}
                        step={RULE_LIMITS[f].step}
                        value={rule[f] ?? ""}
                        onChange={(e) => setField(f, e.target.value === "" ? undefined : Number(e.target.value))}
                      />
                      <span className="font-mono text-xs text-parch-500">
                        {RULE_LIMITS[f].unit} ({RULE_LIMITS[f].min}..{RULE_LIMITS[f].max})
                      </span>
                    </>
                  ))}
              </div>
            );
          })}

          <label className="mt-2 block">
            <span className="mb-1 block font-display text-xs uppercase tracking-widest text-parch-400">Note</span>
            <Textarea
              rows={2}
              value={rule._note ?? ""}
              onChange={(e) => setRule((r) => ({ ...r, _note: e.target.value || undefined }))}
              placeholder="why this rule exists, for the next person"
            />
          </label>
        </div>

        {(errors.length > 0 || nameClash) && (
          <div className="chamfer-sm mt-3 bg-stone-850 p-2">
            {nameClash && (
              <div className="flex items-start gap-1.5 text-xs text-gold-400">
                <CircleAlert className="mt-0.5 h-3.5 w-3.5 shrink-0" />
                A rule named {ruleName} already exists -- saving will overwrite it.
              </div>
            )}
            {errors.map((e) => (
              <div key={e} className="flex items-start gap-1.5 text-xs text-rust-400">
                <CircleAlert className="mt-0.5 h-3.5 w-3.5 shrink-0" /> {e}
              </div>
            ))}
          </div>
        )}

        <div className="mt-4 flex justify-end gap-2">
          <Button variant="ghost" onClick={onClose}>Cancel</Button>
          <Button variant="gold" disabled={errors.length > 0 || busy} onClick={() => void save()}>
            Write to the library
          </Button>
        </div>
      </DialogContent>
    </Dialog>
  );
}

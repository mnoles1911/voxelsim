/* THE SCHEMA SEAM.
 *
 * Everything the UI knows about the placement-spec contract lives in this one
 * file, so when docs/placement-spec-schema.md moves, the adaptation is here
 * and nowhere else. The shape mirrors the server-side authorities:
 *
 *   forge/spec.py        `biome_allow`, `biome_rules`, `curation`, RULE_FIELDS
 *   forge/biomes.py      the biome menu + `allowed()` (allowlist resolver)
 *   rules/placement-rules.json   the named-rule library
 *   rules/biome-density.json     the kind x biome density table (read-only here)
 *
 * Biome keys, kind keys and rule names are NEVER hardcoded in the UI -- they
 * are fetched from the server, which reads forge/biomes.py and forge/kinds.py.
 * This file only fixes the FIELD LAW: which fields a rule may carry, their
 * clamps, and how several attached rules compose.
 */

/* --- server vocabulary (fetched, never invented) ------------------------ */

export interface Biome {
  id: number;
  key: string;
  label: string;
  surface: string;
  climate: string;
  plantable: boolean;
  hosts: string[]; // kind keys this biome hosts
}

export interface Kind {
  key: string;
  label: string;
  blurb: string;
  ready: boolean;
  species: number;
}

/* --- curation (mirrors forge/spec.py curation block) -------------------- */

export const CURATION_STATUSES = ["draft", "approved", "rejected"] as const;
export type CurationStatus = (typeof CURATION_STATUSES)[number];
export const CURATION_SEEDS = [1, 2, 3, 4] as const; // the default bank seeds

export interface Curation {
  status: CurationStatus;
  seeds: number[];
  notes: string;
  /* false = grandfathered: no human has looked, yet it exports (the gate's
   * grandfather clause -- absent means approved at seeds 1-4). */
  curated: boolean;
}

/* --- named placement rules (mirrors RULE_FIELDS in forge/spec.py) ------- */

export const WATER_KINDS = ["any", "ocean", "river", "lake", "shallow", "reef"] as const;
export type WaterKind = (typeof WATER_KINDS)[number];

export interface PlacementRule {
  elev_min_m?: number;
  elev_max_m?: number;
  slope_min_pct?: number;
  slope_max_pct?: number;
  water_max_m?: number;
  water?: WaterKind;
  spacing_m?: number;
  abundance?: number;
  cluster?: number;
  _note?: string;
}

export const RULE_FIELDS = [
  "elev_min_m", "elev_max_m", "slope_min_pct", "slope_max_pct",
  "water_max_m", "water", "spacing_m", "abundance", "cluster",
] as const;
export type RuleField = (typeof RULE_FIELDS)[number];

/* Rule names are wire data (32-byte ASCII) -- same law as forge/spec.py. */
export const RULE_NAME_RE = /^[A-Za-z0-9_-]{1,31}$/;

/* Clamp ranges, checked client-side before any write reaches the server.
 * Slope tops out at 70% because the engine's bare-rock gate owns anything
 * steeper; unit fractions are genuinely 0..1. Elevation spans the world's
 * plausible band with margin. */
export const RULE_LIMITS: Record<
  Exclude<RuleField, "water">,
  { min: number; max: number; step: number; unit: string }
> = {
  elev_min_m: { min: -500, max: 9000, step: 10, unit: "m" },
  elev_max_m: { min: -500, max: 9000, step: 10, unit: "m" },
  slope_min_pct: { min: 0, max: 70, step: 1, unit: "%" },
  slope_max_pct: { min: 0, max: 70, step: 1, unit: "%" },
  water_max_m: { min: 0.1, max: 2000, step: 5, unit: "m" },
  spacing_m: { min: 0, max: 500, step: 1, unit: "m" },
  abundance: { min: 0, max: 1, step: 0.05, unit: "" },
  cluster: { min: 0, max: 1, step: 0.05, unit: "" },
};

export const RULE_FIELD_LABELS: Record<RuleField, string> = {
  elev_min_m: "Elevation floor",
  elev_max_m: "Elevation ceiling",
  slope_min_pct: "Slope at least",
  slope_max_pct: "Slope at most",
  water_max_m: "Within distance of water",
  water: "Water kind",
  spacing_m: "Minimum spacing",
  abundance: "Abundance",
  cluster: "Cluster strength",
};

/** Validate one rule body. Returns human-readable problems; empty = writable. */
export function validateRule(name: string, rule: PlacementRule): string[] {
  const errs: string[] = [];
  if (!RULE_NAME_RE.test(name))
    errs.push("Name must be 1-31 characters of letters, digits, - or _ (it is wire data).");
  const set = RULE_FIELDS.filter((f) => rule[f] !== undefined);
  if (set.length === 0) errs.push("A rule must set at least one field, or it restricts nothing.");
  for (const f of set) {
    if (f === "water") {
      if (!WATER_KINDS.includes(rule.water as WaterKind))
        errs.push("Water kind must be one of: " + WATER_KINDS.join(", ") + ".");
      continue;
    }
    const v = rule[f];
    const lim = RULE_LIMITS[f];
    if (typeof v !== "number" || Number.isNaN(v)) {
      errs.push(RULE_FIELD_LABELS[f] + " must be a number.");
    } else if (v < lim.min || v > lim.max) {
      errs.push(
        (RULE_FIELD_LABELS[f] + " must be between " + lim.min + " and " + lim.max + " " + lim.unit).trim() + ".",
      );
    }
  }
  if (rule.elev_min_m !== undefined && rule.elev_max_m !== undefined && rule.elev_min_m >= rule.elev_max_m)
    errs.push("Elevation floor must sit below the ceiling.");
  if (rule.slope_min_pct !== undefined && rule.slope_max_pct !== undefined && rule.slope_min_pct >= rule.slope_max_pct)
    errs.push("Minimum slope must sit below the maximum.");
  if (rule.water !== undefined && rule.water_max_m === undefined)
    errs.push("A water kind needs a distance (set 'Within distance of water').");
  return errs;
}

/** Compose several attached rules the way the exporter will: by INTERSECTION,
 * strictest wins, because every gate is veto-only. Used to PREVIEW the
 * effective restriction under an attachment list; never written anywhere. */
export function composeRules(rules: PlacementRule[]): { effective: PlacementRule; conflicts: string[] } {
  const out: PlacementRule = {};
  const conflicts: string[] = [];
  const maxOf = (f: "elev_min_m" | "slope_min_pct" | "spacing_m") => {
    const vs = rules.map((r) => r[f]).filter((v): v is number => v !== undefined);
    if (vs.length) out[f] = Math.max(...vs);
  };
  const minOf = (f: "elev_max_m" | "slope_max_pct" | "water_max_m" | "abundance") => {
    const vs = rules.map((r) => r[f]).filter((v): v is number => v !== undefined);
    if (vs.length) out[f] = Math.min(...vs);
  };
  maxOf("elev_min_m");
  maxOf("slope_min_pct");
  maxOf("spacing_m");
  minOf("elev_max_m");
  minOf("slope_max_pct");
  minOf("water_max_m");
  minOf("abundance");
  const waters = [...new Set(rules.map((r) => r.water).filter(Boolean))] as WaterKind[];
  if (waters.length === 1) out.water = waters[0];
  else if (waters.length > 1)
    conflicts.push("Water kinds " + waters.join(" + ") + ": a site must satisfy every one (intersection).");
  const clusters = [...new Set(rules.map((r) => r.cluster).filter((v) => v !== undefined))];
  if (clusters.length === 1) out.cluster = clusters[0] as number;
  else if (clusters.length > 1)
    conflicts.push("Cluster set to " + clusters.join(" and ") + " by different rules -- setters must agree.");
  if (out.elev_min_m !== undefined && out.elev_max_m !== undefined && out.elev_min_m >= out.elev_max_m)
    conflicts.push("Elevation bands do not overlap: the species can place NOWHERE in this biome.");
  if (out.slope_min_pct !== undefined && out.slope_max_pct !== undefined && out.slope_min_pct >= out.slope_max_pct)
    conflicts.push("Slope bands do not overlap: the species can place NOWHERE in this biome.");
  return { effective: out, conflicts };
}

/** One line of plain English for a rule body, for menus and chips. */
export function describeRule(r: PlacementRule): string {
  const bits: string[] = [];
  if (r.elev_min_m !== undefined || r.elev_max_m !== undefined)
    bits.push("elevation " + (r.elev_min_m ?? "…") + "-" + (r.elev_max_m ?? "…") + " m");
  if (r.slope_min_pct !== undefined || r.slope_max_pct !== undefined)
    bits.push("slope " + (r.slope_min_pct ?? 0) + "-" + (r.slope_max_pct ?? 70) + "%");
  if (r.water_max_m !== undefined)
    bits.push("within " + r.water_max_m + " m of " + (r.water && r.water !== "any" ? r.water : "water"));
  if (r.spacing_m !== undefined) bits.push("spacing >= " + r.spacing_m + " m");
  if (r.abundance !== undefined) bits.push("abundance " + r.abundance);
  if (r.cluster !== undefined) bits.push("cluster " + r.cluster);
  return bits.join(", ") || "restricts nothing";
}

/* --- per-species placement (mirrors the spec's blocks) ------------------ */

/** `biome_allow` semantics: absent = derived from the weights; [] = allowed
 * NOWHERE; a list = exactly these biomes. The UI's three allowlist modes. */
export type AllowlistMode = "derived" | "explicit" | "nowhere";

export function allowlistMode(biome_allow: string[] | null | undefined): AllowlistMode {
  if (biome_allow == null) return "derived";
  return biome_allow.length === 0 ? "nowhere" : "explicit";
}

/** Mirrors forge.biomes.allowed(): explicit list clipped to hosting biomes;
 * otherwise derived from positive weights. Preview only -- the server-side
 * resolver is the authority. */
export function allowedBiomes(
  spec: { kind: string; weights: Record<string, number>; biome_allow: string[] | null },
  biomes: Biome[],
): string[] {
  const hosts = biomes.filter((b) => b.hosts.includes(spec.kind)).map((b) => b.key);
  if (spec.biome_allow != null) return spec.biome_allow.filter((k) => hosts.includes(k));
  return hosts.filter((k) => (spec.weights[k] ?? 0) > 0);
}

export interface SpeciesRow {
  name: string;
  file: string;
  kind: string;
  hash: string;
  size_m: number;
  resolution_cm: number;
  shape: string;
  notes: string;
  biomes: string; // human summary from the server
  curation: Curation;
  weights: Record<string, number>; // biome key -> 0..1 (zeroes dropped)
  biome_allow: string[] | null; // null = derived
  biome_rules: Record<string, string[]>; // biome key -> attached rule names
}

export interface LibraryEntry {
  id: string;
  species: string;
  kind: string;
  seed: number;
  spec_hash?: string;
  imported?: boolean;
  stats?: Record<string, number>;
  problems?: string[];
}

export interface RulesDoc {
  rules: Record<string, PlacementRule>;
  density: Record<string, Record<string, number>>; // kind -> biome -> 0..1
  referenced: Record<string, string[]>; // rule name -> species using it
}

/* --- the authoring schema (mirrors forge/spec.py ui_schema()) ------------ */

/** One parameter row from GET /api/schema?kind=... The parameter panel is
 * GENERATED from these -- never hand-written per kind -- so a new kind's
 * sliders appear the moment its generator lands server-side. */
export interface UiParam {
  path: string; // dotted path into the spec
  label: string;
  kind: "float" | "int" | "bool" | "choice" | "text";
  default: unknown;
  lo: number;
  hi: number;
  step: number;
  choices: string[];
  group: string;
  help: string;
}

export interface UiSchema {
  kind: string | null;
  params: UiParam[];
  groups: string[]; // schema group order, already scoped to the kind
}

/** A live spec draft is a plain nested object; these are the two dotted-path
 * helpers the whole authoring surface shares. */
export const getPath = (obj: unknown, path: string): unknown =>
  path.split(".").reduce<unknown>((o, k) => (o == null ? undefined : (o as Record<string, unknown>)[k]), obj);

export function setPath(obj: Record<string, unknown>, path: string, value: unknown): void {
  const keys = path.split(".");
  let node: Record<string, unknown> = obj;
  for (const k of keys.slice(0, -1)) {
    if (typeof node[k] !== "object" || node[k] == null) node[k] = {};
    node = node[k] as Record<string, unknown>;
  }
  node[keys[keys.length - 1]] = value;
}

/* --- generation jobs (mirrors Job.progress() in forge/server.py) --------- */

export interface TileState {
  ready: boolean;
  error: string | null;
  stats: Record<string, unknown> | null;
  problems: string[];
}

export interface JobProgress {
  job: string;
  done: number;
  total: number;
  seeds: number[];
  tiles: Record<string, TileState>;
}

/** /api/interpret: the LOCAL plain-language spec editor (forge/language.py).
 * No model callbacks, by standing constraint -- the vocabulary is authored
 * in the repo and the route never leaves the machine. */
export interface InterpretResult {
  spec: Record<string, unknown>;
  understood: string[];
  ignored: string[];
  edits: { label: string; from: unknown; to: unknown }[];
  warnings?: string[];
}

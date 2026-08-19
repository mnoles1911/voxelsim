/* Typed fetch wrappers over forge/server.py. Every shape comes from
 * lib/schema.ts (the seam); no route is called anywhere else in the app. */

import type {
  Biome, Curation, CurationStatus, Kind, LibraryEntry, PlacementRule, RulesDoc, SpeciesRow,
} from "./schema";

async function j<T>(r: Response): Promise<T> {
  const body = await r.json().catch(() => ({ error: r.statusText }));
  if (!r.ok || (body && typeof body === "object" && "error" in body && (body as { error?: string }).error)) {
    throw new Error((body as { error?: string }).error ?? "HTTP " + r.status);
  }
  return body as T;
}

const post = (url: string, body: unknown) =>
  fetch(url, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });

export const api = {
  biomes: () => fetch("/api/biomes").then((r) => j<Biome[]>(r)),
  kinds: () => fetch("/api/kinds").then((r) => j<Kind[]>(r)),
  specs: () => fetch("/api/specs").then((r) => j<SpeciesRow[]>(r)),
  library: () => fetch("/api/library").then((r) => j<LibraryEntry[]>(r)),
  rules: () => fetch("/api/rules").then((r) => j<RulesDoc>(r)),
  palette: () => fetch("/api/palette").then((r) => j<Record<string, [number, number, number]>>(r)),

  /* Curation writes mirror the server contract: the spec FILE is the record,
   * and only the curation block moves. */
  saveCuration: (name: string, status: CurationStatus, seeds: number[], notes: string) =>
    post("/api/curation", { name, status, seeds, notes }).then((r) =>
      j<{ saved: string; curation: Curation }>(r)),

  /* Placement writes: each block present in the payload replaces that block in
   * the raw spec file and NOTHING else moves (same law as /api/curation).
   * `biome_allow: null` removes the block (back to derived); `biome_rules`
   * entries with empty lists are dropped server-side. */
  savePlacement: (
    name: string,
    payload: {
      weights?: Record<string, number>;
      biome_allow?: string[] | null;
      biome_rules?: Record<string, string[]> | null;
    },
  ) =>
    post("/api/placement", { name, ...payload }).then((r) =>
      j<{
        saved: string;
        weights: Record<string, number>;
        biome_allow: string[] | null;
        biome_rules: Record<string, string[]>;
        allowed: string[];
        warnings: string[];
      }>(r)),

  saveRule: (name: string, rule: PlacementRule) =>
    post("/api/rules/save", { name, rule }).then((r) => j<{ saved: string; rules: RulesDoc["rules"] }>(r)),

  deleteRule: (name: string) =>
    post("/api/rules/delete", { name }).then((r) => j<{ deleted: string; rules: RulesDoc["rules"] }>(r)),

  /* Import an asset from an outside 3D source into the library. The server
   * gives it a spec file too, so placement works identically to generated
   * assets -- the UI never needs to know which path an asset arrived by. */
  importAsset: (payload: {
    name: string;
    kind: string;
    format: "vox" | "vxa";
    data_b64: string;
    voxel_mm?: number;
  }) => post("/api/import", payload).then((r) => j<LibraryEntry>(r)),

  deleteLibrary: (id: string) => post("/api/library/delete", { id }).then((r) => j<{ deleted: string }>(r)),

  thumbUrl: (id: string) => "/api/library/thumb?id=" + encodeURIComponent(id),
  voxelsUrl: (id: string, budget?: number) =>
    "/api/voxels?id=" + encodeURIComponent(id) + (budget ? "&max=" + budget : ""),
  downloadUrl: (id: string, fmt: "vox" | "vxa" | "spec") =>
    "/api/download?id=" + encodeURIComponent(id) + "&fmt=" + fmt,
};

/* --- authoring: generation, adjustment, keeping (stages 1-3) ------------- */

import type { InterpretResult, JobProgress, UiSchema } from "./schema";

export const forgeApi = {
  schema: (kind: string) => fetch("/api/schema?kind=" + encodeURIComponent(kind)).then((r) => j<UiSchema>(r)),

  spec: (name: string) =>
    fetch("/api/spec?name=" + encodeURIComponent(name)).then((r) =>
      j<{ spec: Record<string, unknown>; warnings: string[]; hash: string }>(r)),

  librarySpec: (id: string) =>
    fetch("/api/library/spec?id=" + encodeURIComponent(id)).then((r) =>
      j<{ spec: Record<string, unknown>; seed: number; hash: string }>(r)),

  generate: (spec: Record<string, unknown>, seed_start: number, count: number) =>
    post("/api/generate", { spec, seed_start, count }).then((r) =>
      j<{ job: string; seeds: number[]; warnings: string[]; hash: string }>(r)),

  job: (id: string) => fetch("/api/job?job=" + encodeURIComponent(id)).then((r) => j<JobProgress>(r)),

  keep: (spec: Record<string, unknown>, seed: number) =>
    post("/api/keep", { spec, seed }).then((r) =>
      j<{ id: string; species: string; kind: string; seed: number; vox_models: number }>(r)),

  saveSpec: (spec: Record<string, unknown>) =>
    post("/api/save-spec", { spec }).then((r) => j<{ saved: string; warnings: string[] }>(r)),

  /* Plain-language edits: fully LOCAL (forge/language.py) -- the request goes
   * to the forge server on this machine and nowhere else. */
  interpret: (spec: Record<string, unknown>, request: string) =>
    post("/api/interpret", { spec, request }).then((r) => j<InterpretResult>(r)),

  vocabulary: () =>
    fetch("/api/vocabulary").then((r) => j<{ concepts: unknown[] }>(r)),

  tileUrl: (job: string, seed: number) => "/api/tile?job=" + encodeURIComponent(job) + "&seed=" + seed,
  detailUrl: (job: string, seed: number) => "/api/detail?job=" + encodeURIComponent(job) + "&seed=" + seed,
  jobVoxelsUrl: (job: string, seed: number) => "/api/voxels?job=" + encodeURIComponent(job) + "&seed=" + seed,
};

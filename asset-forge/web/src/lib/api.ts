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

import * as React from "react";
import { FileUp, Import } from "lucide-react";
import type { World } from "../App";
import { api } from "../lib/api";
import type { LibraryEntry } from "../lib/schema";
import { Button } from "./ui/button";
import { Dialog, DialogContent, DialogDescription, DialogTitle } from "./ui/dialog";
import { Input } from "./ui/input";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "./ui/select";

/* IMPORT: assets from outside 3D sources (MagicaVoxel .vox or the forge's
 * own .vxa) saved into the library. The server also creates a spec file for
 * the species, so an imported asset is curated and placement-specced through
 * exactly the same panels as a generated one. */

const NAME_RE = /^[a-z0-9][a-z0-9-]{0,39}$/;

export function ImportDialog({
  open, onOpenChange, world, onImported,
}: {
  open: boolean;
  onOpenChange: (o: boolean) => void;
  world: World;
  onImported: (entry: LibraryEntry) => Promise<void>;
}) {
  const [file, setFile] = React.useState<File | null>(null);
  const [name, setName] = React.useState("");
  const [kind, setKind] = React.useState("rock");
  const [voxelMm, setVoxelMm] = React.useState(100);
  const [busy, setBusy] = React.useState(false);
  const [error, setError] = React.useState<string | null>(null);

  const format = file?.name.toLowerCase().endsWith(".vxa") ? "vxa" : "vox";
  const problems: string[] = [];
  if (file && !/\.(vox|vxa)$/i.test(file.name)) problems.push("Only .vox (MagicaVoxel) and .vxa files import.");
  if (name && !NAME_RE.test(name)) problems.push("Species name: lowercase letters, digits and dashes, up to 40.");
  if (format === "vox" && (voxelMm < 10 || voxelMm > 1000)) problems.push("Voxel size must be 10-1000 mm.");
  const ready = !!file && NAME_RE.test(name) && problems.length === 0;

  const run = async () => {
    if (!file) return;
    setBusy(true);
    setError(null);
    try {
      const buf = await file.arrayBuffer();
      let bin = "";
      const bytes = new Uint8Array(buf);
      const CHUNK = 0x8000;
      for (let i = 0; i < bytes.length; i += CHUNK)
        bin += String.fromCharCode(...bytes.subarray(i, i + CHUNK));
      const entry = await api.importAsset({
        name, kind, format, data_b64: btoa(bin),
        ...(format === "vox" ? { voxel_mm: voxelMm } : {}),
      });
      await onImported(entry);
      onOpenChange(false);
      setFile(null);
      setName("");
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  };

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent>
        <DialogTitle className="flex items-center gap-2">
          <Import className="h-5 w-5 text-gold-400" /> Import an asset
        </DialogTitle>
        <DialogDescription>
          Bring a voxel model from an outside source into the library. It gets a species entry like any
          generated asset: same verdict, same biomes, same allowlist, same rule overrides.
        </DialogDescription>

        <div className="flex flex-col gap-3">
          <label className="chamfer bevel-down flex cursor-pointer items-center gap-3 bg-stone-850 p-3">
            <FileUp className="h-5 w-5 text-parch-400" />
            <span className="text-sm text-parch-300">
              {file ? file.name + " (" + (file.size / 1024).toFixed(0) + " KB)" : "Choose a .vox or .vxa file…"}
            </span>
            <input
              type="file"
              accept=".vox,.vxa"
              className="hidden"
              onChange={(e) => {
                const f = e.target.files?.[0] ?? null;
                setFile(f);
                if (f && !name)
                  setName(
                    f.name.replace(/\.(vox|vxa)$/i, "").toLowerCase().replace(/[^a-z0-9-]+/g, "-").replace(/^-+|-+$/g, ""),
                  );
              }}
            />
          </label>

          <div className="grid grid-cols-2 gap-3">
            <label className="block">
              <span className="mb-1 block font-display text-xs uppercase tracking-widest text-parch-400">
                Species name
              </span>
              <Input value={name} onChange={(e) => setName(e.target.value)} placeholder="granite-shrine" className="font-mono" />
            </label>
            <label className="block">
              <span className="mb-1 block font-display text-xs uppercase tracking-widest text-parch-400">Kind</span>
              <Select value={kind} onValueChange={setKind}>
                <SelectTrigger><SelectValue /></SelectTrigger>
                <SelectContent>
                  {world.kinds.map((k) => (
                    <SelectItem key={k.key} value={k.key}>{k.label}</SelectItem>
                  ))}
                </SelectContent>
              </Select>
            </label>
          </div>

          {format === "vox" && (
            <label className="block w-48">
              <span className="mb-1 block font-display text-xs uppercase tracking-widest text-parch-400">
                Voxel size (mm)
              </span>
              <Input type="number" min={10} max={1000} step={10} value={voxelMm} onChange={(e) => setVoxelMm(Number(e.target.value))} />
            </label>
          )}

          {(problems.length > 0 || error) && (
            <div className="chamfer-sm bg-stone-850 p-2 text-xs text-rust-400">
              {problems.map((p) => (
                <div key={p}>{p}</div>
              ))}
              {error && <div>{error}</div>}
            </div>
          )}

          <div className="flex justify-end gap-2">
            <Button variant="ghost" onClick={() => onOpenChange(false)}>Cancel</Button>
            <Button variant="gold" disabled={!ready || busy} onClick={() => void run()}>
              {busy ? "Importing…" : "Import into the library"}
            </Button>
          </div>
        </div>
      </DialogContent>
    </Dialog>
  );
}

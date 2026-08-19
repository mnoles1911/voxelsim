import * as React from "react";
import { RotateCcw } from "lucide-react";
import { api } from "../lib/api";
import { createViewer, decodeVoxels, type VoxelViewer } from "../lib/viewer";
import { Button } from "./ui/button";

/* The ported 3D variant viewer: one instanced WebGL2 draw of the entry's
 * surface voxels (see lib/viewer.ts). Drag orbits, wheel/pinch zooms,
 * double-click goes home. */

export function VariantViewer({
  entryId, palette,
}: {
  entryId: string;
  palette: Record<string, [number, number, number]>;
}) {
  const canvasRef = React.useRef<HTMLCanvasElement>(null);
  const viewerRef = React.useRef<VoxelViewer | null>(null);
  const [status, setStatus] = React.useState<string>("loading…");
  const token = React.useRef(0);

  React.useEffect(() => {
    const v = canvasRef.current ? createViewer(canvasRef.current) : null;
    viewerRef.current = v;
    if (!v) setStatus("WebGL2 unavailable — use the flat thumbnail");
    return () => v?.dispose();
  }, []);

  React.useEffect(() => {
    const v = viewerRef.current;
    if (!v) return;
    const mine = ++token.current;
    setStatus("loading…");
    fetch(api.voxelsUrl(entryId))
      .then(async (r) => {
        if (!r.ok) throw new Error((await r.json().catch(() => null))?.error ?? "HTTP " + r.status);
        const cm = r.headers.get("X-Voxel-Cm");
        const buf = await r.arrayBuffer();
        if (token.current !== mine) return; // a newer selection landed first
        const { offsets, colors, dims, count } = decodeVoxels(buf, palette);
        v.setInstances(offsets, colors, dims);
        setStatus(count.toLocaleString() + " voxels" + (cm ? " at " + cm + " cm" : ""));
      })
      .catch((e) => token.current === mine && setStatus("failed: " + e.message));
  }, [entryId, palette]);

  return (
    <div className="relative">
      <canvas ref={canvasRef} className="chamfer bevel-down block h-80 w-full touch-none bg-stone-850" />
      <div className="absolute bottom-2 left-3 font-mono text-[11px] text-parch-400">{status}</div>
      <Button
        variant="ghost"
        size="icon"
        className="absolute right-2 top-2 h-7 w-7"
        title="Reset view"
        onClick={() => viewerRef.current?.reset()}
      >
        <RotateCcw className="h-4 w-4" />
      </Button>
    </div>
  );
}

import * as React from "react";
import { RotateCcw, ZoomIn, ZoomOut } from "lucide-react";
import { createViewer, decodeVoxels, type VoxelViewer } from "../lib/viewer";
import { Button } from "./ui/button";
import { cn } from "../lib/cn";

/* The ported 3D voxel viewer: one instanced WebGL2 draw of the asset's
 * surface voxels (see lib/viewer.ts). Drag orbits, wheel/pinch zooms,
 * double-click goes home -- and the controls are VISIBLE, not implied:
 * zoom in / zoom out / reset buttons plus a spoken gesture hint. Switching
 * `src` keeps the camera (angles and zoom ratio), so stepping between a
 * species' seeds compares them from the same viewpoint. */

export interface DecodedInfo {
  count: number;
  dims: [number, number, number];
  materialCounts: Record<number, number>;
  cm: number | null; // voxel pitch the server actually sent
  authoredCm: number | null;
}

export function VoxelCanvas({
  src, palette, className, onDecoded,
}: {
  src: string;
  palette: Record<string, [number, number, number]>;
  className?: string;
  onDecoded?: (info: DecodedInfo) => void;
}) {
  const canvasRef = React.useRef<HTMLCanvasElement>(null);
  const viewerRef = React.useRef<VoxelViewer | null>(null);
  const [status, setStatus] = React.useState<string>("loading…");
  const token = React.useRef(0);
  const onDecodedRef = React.useRef(onDecoded);
  onDecodedRef.current = onDecoded;

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
    fetch(src)
      .then(async (r) => {
        if (!r.ok) throw new Error((await r.json().catch(() => null))?.error ?? "HTTP " + r.status);
        const cm = r.headers.get("X-Voxel-Cm");
        const authored = r.headers.get("X-Authored-Cm");
        const buf = await r.arrayBuffer();
        if (token.current !== mine) return; // a newer selection landed first
        const { offsets, colors, dims, count, materialCounts } = decodeVoxels(buf, palette);
        v.setInstances(offsets, colors, dims);
        onDecodedRef.current?.({
          count,
          dims,
          materialCounts,
          cm: cm ? Number(cm) : null,
          authoredCm: authored ? Number(authored) : null,
        });
        // Say plainly when the preview lattice is coarser than the asset is
        // authored at, so nobody mistakes it for the export.
        const note =
          cm && authored && cm !== authored
            ? " · shown at " + cm + " cm (exports at " + authored + " cm)"
            : cm
              ? " · " + cm + " cm voxels"
              : "";
        setStatus(count.toLocaleString() + " voxels" + note);
      })
      .catch((e) => token.current === mine && setStatus("failed: " + e.message));
  }, [src, palette]);

  return (
    <div className="relative">
      <canvas
        ref={canvasRef}
        className={cn("chamfer bevel-down block h-80 w-full touch-none bg-stone-850", className)}
      />
      <div className="absolute bottom-2 left-3 font-mono text-[11px] text-parch-400">
        {status}
        <span className="ml-2 text-parch-600">drag to orbit · scroll to zoom · double-click resets</span>
      </div>
      <div className="absolute right-2 top-2 flex flex-col gap-1">
        <Button variant="ghost" size="icon" className="h-7 w-7 bg-stone-800/70" title="Zoom in"
          onClick={() => viewerRef.current?.zoom(1 / 1.3)}>
          <ZoomIn className="h-4 w-4" />
        </Button>
        <Button variant="ghost" size="icon" className="h-7 w-7 bg-stone-800/70" title="Zoom out"
          onClick={() => viewerRef.current?.zoom(1.3)}>
          <ZoomOut className="h-4 w-4" />
        </Button>
        <Button variant="ghost" size="icon" className="h-7 w-7 bg-stone-800/70" title="Reset view"
          onClick={() => viewerRef.current?.reset()}>
          <RotateCcw className="h-4 w-4" />
        </Button>
      </div>
    </div>
  );
}

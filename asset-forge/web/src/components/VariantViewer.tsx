import * as React from "react";
import { RotateCcw, ZoomIn, ZoomOut, UserRound } from "lucide-react";
import { createViewer, decodeVoxels, type VoxelViewer } from "../lib/appearance-viewer";
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
  src, palette, className, wrapperClassName, onDecoded,
}: {
  src: string;
  palette: Record<string, [number, number, number]>;
  className?: string;
  /** Size the OUTER box instead of the canvas (e.g. "min-h-0 flex-1" inside
   *  a resizable panel); pair with className="h-full". The viewer re-reads
   *  clientWidth/Height every frame, so live resizing just works. */
  wrapperClassName?: string;
  onDecoded?: (info: DecodedInfo) => void;
}) {
  const canvasRef = React.useRef<HTMLCanvasElement>(null);
  const viewerRef = React.useRef<VoxelViewer | null>(null);
  const [status, setStatus] = React.useState<string>("loading…");
  const [pawnVisible, setPawnVisible] = React.useState(() => {
    try { return localStorage.getItem("af-preview-pawn") !== "off"; }
    catch { return true; }
  });
  const [hasScale, setHasScale] = React.useState(false);
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
    viewerRef.current?.setPawnVisible(pawnVisible);
    try { localStorage.setItem("af-preview-pawn", pawnVisible ? "on" : "off"); }
    catch { /* The toggle works even when storage is unavailable. */ }
  }, [pawnVisible]);

  React.useEffect(() => {
    const v = viewerRef.current;
    if (!v) return;
    const mine = ++token.current;
    setStatus("loading…");
    fetch(src+(src.includes('?')?'&':'?')+'appearance=2')
      .then(async (r) => {
        if (!r.ok) throw new Error((await r.json().catch(() => null))?.error ?? "HTTP " + r.status);
        const cm = r.headers.get("X-Voxel-Cm");
        const authored = r.headers.get("X-Authored-Cm");
        const buf = await r.arrayBuffer();
        if (token.current !== mine) return; // a newer selection landed first
        const { offsets, colors, dims, count, materialCounts } = decodeVoxels(buf, palette);
        const pitch = cm ? Number(cm) : undefined;
        const treeAppearance=r.headers.get('X-Tree-Appearance');
        const leafMaterials=new Uint8Array(buf,16+count*6,count);
        const foliage=treeAppearance?Float32Array.from(leafMaterials,m=>[19,20,21,22,24,25].includes(m)?1:0):undefined;
        setHasScale(!!pitch && Number.isFinite(pitch) && pitch > 0);
        v.setInstances(offsets, colors, dims, pitch,foliage);
        v.setVariation(treeAppearance?1:0);
        v.setMask(!!treeAppearance && r.headers.get('X-Tree-Foliage')!=='none',r.headers.get('X-Tree-Foliage')==='needle',.45);
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
    <div className={cn("relative", wrapperClassName)}>
      <canvas
        ref={canvasRef}
        className={cn("chamfer bevel-down block h-80 w-full touch-none bg-stone-850", className)}
      />
      <div className="absolute bottom-2 left-3 font-mono text-[11px] text-parch-400">
        {status}
        <span className="ml-2 text-parch-600">drag to orbit · scroll to zoom · double-click resets</span>
      </div>
      <div className="absolute right-2 top-2 flex flex-col gap-1">
        <Button variant="ghost" size="icon"
          className={cn("h-7 w-7 bg-stone-800/70", pawnVisible && hasScale && "text-gold-400 ring-1 ring-gold-600")}
          title={hasScale ? (pawnVisible ? "Hide player pawn (1.8 m)" : "Show player pawn (1.8 m)") : "Player scale unavailable"}
          aria-label="Player pawn (1.8 m)" aria-pressed={pawnVisible && hasScale} disabled={!hasScale}
          onClick={() => setPawnVisible(v => !v)}>
          <UserRound className="h-4 w-4" />
        </Button>
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

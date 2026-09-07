import * as React from "react";
import { X } from "lucide-react";
import { cn } from "../../lib/cn";

/* A DRAGGABLE, RESIZABLE floating panel (owner directive 2026-09-05: the
 * variant view "should be twice as big", resizable and draggable).
 *
 * Not a Radix Dialog on purpose: Radix centres its content with a CSS
 * transform, and a drag implementation fighting that translate is exactly
 * the kind of two-owners-of-one-position bug this codebase keeps writing
 * down. This panel owns its box outright: position and size live in state,
 * persist to localStorage under `storageKey`, and clamp to the viewport on
 * every change so a saved position from a bigger monitor cannot strand the
 * panel off-screen.
 *
 * DRAGGING NEVER TOUCHES THE CONTENT. Only the title bar moves the panel
 * and only the corner handle resizes it, so the 3D canvas keeps its own
 * pointer gestures (orbit drag, wheel zoom) untouched -- the two drags are
 * bound to disjoint elements and cannot steal from each other. */

export interface PanelBox {
  x: number;
  y: number;
  w: number;
  h: number;
}

const MIN_W = 520;
const MIN_H = 380;

function clampBox(b: PanelBox): PanelBox {
  const vw = window.innerWidth;
  const vh = window.innerHeight;
  const w = Math.min(Math.max(b.w, MIN_W), vw - 24);
  const h = Math.min(Math.max(b.h, MIN_H), vh - 24);
  return {
    w,
    h,
    x: Math.min(Math.max(b.x, 12 - w * 0.5), vw - w * 0.5 - 12),
    y: Math.min(Math.max(b.y, 12), vh - 64),
  };
}

function loadBox(key: string, fallback: () => PanelBox): PanelBox {
  try {
    const raw = localStorage.getItem(key);
    if (raw) {
      const b = JSON.parse(raw) as PanelBox;
      if ([b.x, b.y, b.w, b.h].every((n) => Number.isFinite(n))) return clampBox(b);
    }
  } catch {
    /* private mode / blocked storage: the default is always usable */
  }
  return clampBox(fallback());
}

export function FloatingPanel({
  storageKey, defaultSize, defaultPosition = "center", title, onClose, children, className,
}: {
  /** localStorage key: the user's size/position survive across sessions.
   *  VERSION IT (-v2, -v3...) whenever the default changes on an owner
   *  directive, so the new default beats any previously-persisted box. */
  storageKey: string;
  /** Default box; compute it from window.* at the call site for
   *  viewport-relative sizing ("half the screen"). Clamped either way. */
  defaultSize: { w: number; h: number };
  /** "right" parks the default against the right edge (the gallery stays
   *  visible beside it); "center" centres it. Only the DEFAULT -- the
   *  user's own dragging wins once persisted. */
  defaultPosition?: "center" | "right";
  title: React.ReactNode;
  onClose: () => void;
  children: React.ReactNode;
  className?: string;
}) {
  const [box, setBox] = React.useState<PanelBox>(() =>
    loadBox(storageKey, () => {
      const w = Math.min(defaultSize.w, window.innerWidth - 24);
      const h = Math.min(defaultSize.h, window.innerHeight - 24);
      return {
        w: defaultSize.w,
        h: defaultSize.h,
        x: defaultPosition === "right" ? window.innerWidth - w - 12
          : (window.innerWidth - w) / 2,
        y: Math.max(12, (window.innerHeight - h) / 2),
      };
    }),
  );
  const boxRef = React.useRef(box);
  boxRef.current = box;

  const persist = React.useCallback(() => {
    try {
      localStorage.setItem(storageKey, JSON.stringify(boxRef.current));
    } catch {
      /* nothing to do: the panel still works, it just forgets */
    }
  }, [storageKey]);

  React.useEffect(() => {
    const onKey = (e: KeyboardEvent) => e.key === "Escape" && onClose();
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [onClose]);

  const track = (
    e: React.PointerEvent,
    apply: (dx: number, dy: number, start: PanelBox) => PanelBox,
  ) => {
    e.preventDefault();
    const sx = e.clientX;
    const sy = e.clientY;
    const start = { ...boxRef.current };
    const move = (ev: PointerEvent) =>
      setBox(clampBox(apply(ev.clientX - sx, ev.clientY - sy, start)));
    const up = () => {
      window.removeEventListener("pointermove", move);
      window.removeEventListener("pointerup", up);
      persist();
    };
    window.addEventListener("pointermove", move);
    window.addEventListener("pointerup", up);
  };

  const startDrag = (e: React.PointerEvent) => {
    // Buttons in the title bar (close, etc.) stay buttons.
    if ((e.target as HTMLElement).closest("button")) return;
    track(e, (dx, dy, s) => ({ ...s, x: s.x + dx, y: s.y + dy }));
  };
  const startResize = (e: React.PointerEvent) =>
    track(e, (dx, dy, s) => ({ ...s, w: s.w + dx, h: s.h + dy }));

  return (
    <>
      {/* dim the app behind; click outside closes, as the dialogs did */}
      <div className="fixed inset-0 z-50 bg-stone-950/60" onClick={onClose} />
      <div
        className={cn(
          "chamfer bevel-up fixed z-50 flex flex-col bg-stone-800",
          "shadow-[8px_8px_0_0_rgb(0_0_0/0.5)]",
          className,
        )}
        style={{ left: box.x, top: box.y, width: box.w, height: box.h }}
        role="dialog"
        aria-modal="true"
      >
        <div
          onPointerDown={startDrag}
          className="mortar-b flex shrink-0 cursor-move select-none items-center gap-2 px-4 py-2.5 font-display text-lg tracking-wide text-parch-100"
          title="Drag to move the panel"
        >
          {title}
          <button
            onClick={onClose}
            className="ml-auto p-1 text-parch-400 hover:text-parch-100"
            title="Close (Esc)"
          >
            <X className="h-4 w-4" />
          </button>
        </div>
        <div className="min-h-0 flex-1 overflow-y-auto p-4">{children}</div>
        {/* the resize corner: its own element, so it can never steal the
          * canvas's orbit drag */}
        <div
          onPointerDown={startResize}
          className="absolute bottom-0 right-0 h-5 w-5 cursor-nwse-resize"
          title="Drag to resize"
        >
          <svg viewBox="0 0 20 20" className="h-full w-full text-parch-500">
            <path d="M18 8v2l-8 8H8zM18 14v2l-2 2h-2z" fill="currentColor" />
          </svg>
        </div>
      </div>
    </>
  );
}

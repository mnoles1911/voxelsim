import * as React from "react";
import { CircleAlert, CircleCheck } from "lucide-react";
import { cn } from "../../lib/cn";

/* A tiny toast rack: no portal library needed for a local tool. `useToast`
 * hands back `toast(msg)` / `toast.error(msg)`; the rack renders bottom-right
 * as stamped stone tablets. */

interface Note {
  id: number;
  msg: string;
  error: boolean;
}

const ToastCtx = React.createContext<{ push: (msg: string, error: boolean) => void } | null>(null);

export function ToastProvider({ children }: { children: React.ReactNode }) {
  const [notes, setNotes] = React.useState<Note[]>([]);
  const push = React.useCallback((msg: string, error: boolean) => {
    const id = Date.now() + Math.random();
    setNotes((n) => [...n, { id, msg, error }]);
    setTimeout(() => setNotes((n) => n.filter((x) => x.id !== id)), error ? 7000 : 3500);
  }, []);
  return (
    <ToastCtx.Provider value={{ push }}>
      {children}
      <div className="pointer-events-none fixed bottom-4 right-4 z-[60] flex w-80 flex-col gap-2">
        {notes.map((n) => (
          <div
            key={n.id}
            className={cn(
              "chamfer-sm bevel-up flex items-start gap-2 px-3 py-2 text-sm shadow-[4px_4px_0_0_rgb(0_0_0/0.45)]",
              n.error ? "bg-rust-600 text-parch-100" : "bg-stone-600 text-parch-100",
            )}
          >
            {n.error ? (
              <CircleAlert className="mt-0.5 h-4 w-4 shrink-0" />
            ) : (
              <CircleCheck className="mt-0.5 h-4 w-4 shrink-0 text-moss-400" />
            )}
            <span className="break-words">{n.msg}</span>
          </div>
        ))}
      </div>
    </ToastCtx.Provider>
  );
}

export function useToast() {
  const ctx = React.useContext(ToastCtx);
  if (!ctx) throw new Error("useToast outside ToastProvider");
  return React.useMemo(
    () => ({
      ok: (msg: string) => ctx.push(msg, false),
      error: (msg: string) => ctx.push(msg, true),
    }),
    [ctx],
  );
}

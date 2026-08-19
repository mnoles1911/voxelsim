import * as React from "react";
import { cn } from "../../lib/cn";

const VARIANTS = {
  default: "bg-stone-600 text-parch-200",
  gold: "bg-gold-600 text-parch-100",
  moss: "bg-moss-600 text-parch-100",
  rust: "bg-rust-600 text-parch-100",
  outline: "bg-transparent text-parch-400 shadow-[inset_0_0_0_1px_var(--color-stone-500)]",
} as const;

export function Badge({
  className,
  variant = "default",
  ...props
}: React.HTMLAttributes<HTMLSpanElement> & { variant?: keyof typeof VARIANTS }) {
  return (
    <span
      className={cn(
        "chamfer-sm inline-flex items-center gap-1 px-1.5 py-0.5 text-[11px] font-display tracking-wide",
        VARIANTS[variant],
        className,
      )}
      {...props}
    />
  );
}

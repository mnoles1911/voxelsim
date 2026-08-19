import * as React from "react";
import { cn } from "../../lib/cn";

/* The forge's buttons: chamfered blocks with a hard bevel, pressed flat on
 * :active. shadcn's API (variant/size), the game's look. */

const VARIANTS = {
  default: "bg-stone-600 text-parch-100 hover:bg-stone-500",
  gold: "bg-gold-600 text-parch-100 hover:bg-gold-500 hover:text-stone-950",
  moss: "bg-moss-600 text-parch-100 hover:bg-moss-500",
  rust: "bg-rust-600 text-parch-100 hover:bg-rust-500",
  ghost: "bg-transparent text-parch-300 hover:bg-stone-700 hover:text-parch-100 shadow-none",
} as const;

const SIZES = {
  default: "h-9 px-4 gap-2 text-sm",
  sm: "h-7 px-2.5 gap-1.5 text-xs",
  lg: "h-11 px-6 gap-2 text-base",
  icon: "h-9 w-9",
} as const;

export interface ButtonProps extends React.ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: keyof typeof VARIANTS;
  size?: keyof typeof SIZES;
}

export const Button = React.forwardRef<HTMLButtonElement, ButtonProps>(
  ({ className, variant = "default", size = "default", type = "button", ...props }, ref) => (
    <button
      ref={ref}
      type={type}
      className={cn(
        "chamfer-sm bevel-up inline-flex select-none items-center justify-center font-display tracking-wide",
        "transition-colors active:bevel-down disabled:pointer-events-none disabled:opacity-40",
        "focus-visible:outline focus-visible:outline-2 focus-visible:outline-gold-500",
        VARIANTS[variant],
        SIZES[size],
        className,
      )}
      {...props}
    />
  ),
);
Button.displayName = "Button";

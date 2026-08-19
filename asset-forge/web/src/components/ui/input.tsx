import * as React from "react";
import { cn } from "../../lib/cn";

export const Input = React.forwardRef<HTMLInputElement, React.InputHTMLAttributes<HTMLInputElement>>(
  ({ className, ...props }, ref) => (
    <input
      ref={ref}
      className={cn(
        "chamfer-sm bevel-down h-9 w-full bg-stone-850 px-3 text-sm text-parch-100",
        "placeholder:text-parch-500 focus-visible:outline focus-visible:outline-2 focus-visible:outline-gold-500",
        "disabled:opacity-40 [appearance:textfield] [&::-webkit-inner-spin-button]:appearance-none",
        className,
      )}
      {...props}
    />
  ),
);
Input.displayName = "Input";

export const Textarea = React.forwardRef<HTMLTextAreaElement, React.TextareaHTMLAttributes<HTMLTextAreaElement>>(
  ({ className, ...props }, ref) => (
    <textarea
      ref={ref}
      className={cn(
        "chamfer-sm bevel-down w-full bg-stone-850 px-3 py-2 text-sm text-parch-100",
        "placeholder:text-parch-500 focus-visible:outline focus-visible:outline-2 focus-visible:outline-gold-500",
        className,
      )}
      {...props}
    />
  ),
);
Textarea.displayName = "Textarea";

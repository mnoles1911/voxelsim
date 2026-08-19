import { clsx, type ClassValue } from "clsx";

/** Class combiner (shadcn convention, minus tailwind-merge: our variants
 * never collide on the same utility). */
export const cn = (...inputs: ClassValue[]) => clsx(inputs);

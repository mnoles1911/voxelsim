import {
  Bird, Box, Fish, Flower2, Mountain, PawPrint, Shell, Sprout, TreePine, Wheat, type LucideIcon,
} from "lucide-react";

/* One icon per asset kind. Lucide only -- no emoji, no raw-text glyphs
 * (repo law). Unknown kinds (the menu comes from the server) fall back to a
 * plain box rather than breaking. */
const ICONS: Record<string, LucideIcon> = {
  tree: TreePine,
  bush: Sprout,
  rock: Mountain,
  grass: Wheat,
  reed: Wheat,
  flower: Flower2,
  fish: Fish,
  cetacean: Shell,
  bird: Bird,
  quadruped: PawPrint,
};

export const kindIcon = (kind: string): LucideIcon => ICONS[kind] ?? Box;

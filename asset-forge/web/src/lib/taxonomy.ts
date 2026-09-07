/** Display taxonomy slugs without exposing generator names as item classes. */
export const groupLabel = (slug: string) => slug.split("-")
  .map((word) => word.charAt(0).toUpperCase() + word.slice(1)).join(" ");

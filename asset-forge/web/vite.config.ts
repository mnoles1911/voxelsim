import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import tailwindcss from "@tailwindcss/vite";

// Dev runs against the live forge server: `python -m forge.cli serve` on 8731,
// then `npm run dev` here and every /api call proxies through. The production
// build is served BY the forge server itself (forge/server.py serves
// web/dist/), so there is still exactly one process in normal use.
export default defineConfig({
  plugins: [react(), tailwindcss()],
  server: { proxy: { "/api": "http://127.0.0.1:8731" } },
  build: { chunkSizeWarningLimit: 900 },
});

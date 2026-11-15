import { readFileSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";
import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// npm run dev: this page on :5173, the API from the running agent on :4213.
// The agent writes its per-run session to ~/.touchdeck/portal.session; the
// proxy adds it to each API call, as the built page does from its <meta>.
const AGENT = "http://127.0.0.1:4213";
const session = () => {
  try {
    return readFileSync(join(homedir(), ".touchdeck", "portal.session"), "utf8").trim();
  } catch {
    return "";
  }
};

export default defineConfig({
  plugins: [react()],
  build: { outDir: "dist", emptyOutDir: true },
  server: {
    host: "127.0.0.1",
    proxy: {
      "/api": {
        target: AGENT,
        changeOrigin: true,
        configure: (proxy) => proxy.on("proxyReq", (req) => req.setHeader("X-TD-Session", session())),
      },
    },
  },
});

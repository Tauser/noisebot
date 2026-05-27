import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import tailwindcss from "@tailwindcss/vite";

export default defineConfig({
  plugins: [react(), tailwindcss()],
  server: {
    allowedHosts: true,
    port: 5173,
    proxy: {
      "/server-api": {
        target: "http://127.0.0.1:8765",
        changeOrigin: true,
        rewrite: (path) => path.replace(/^\/server-api/, ""),
      },
    },
  },
});

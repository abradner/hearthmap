/// <reference types="vitest/config" />
import { defineConfig } from "vite";

// The editor imports the schema and the fictional example from the repo root, so the dev
// server must be allowed to read one level up. base "./" keeps the build relocatable (GitHub
// Pages serves it under /<repo>/).
export default defineConfig({
  base: "./",
  server: {
    fs: { allow: [".."] },
    port: 5174,
    strictPort: true,
  },
  build: {
    target: "es2022",
    sourcemap: true,
  },
  test: {
    include: ["test/**/*.test.ts"],
    environment: "node",
  },
});

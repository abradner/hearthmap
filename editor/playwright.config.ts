import { defineConfig, devices } from "@playwright/test";

// The e2e smoke test drives the real app in Chromium against the Vite dev server.
export default defineConfig({
  testDir: "e2e",
  fullyParallel: false,
  workers: 1,
  forbidOnly: !!process.env.CI,
  retries: 0,
  reporter: process.env.CI ? [["list"], ["html", { open: "never" }]] : "list",
  use: { baseURL: "http://127.0.0.1:5174", trace: "retain-on-failure" },
  projects: [
    { name: "laptop", use: { ...devices["Desktop Chrome"], viewport: { width: 1440, height: 900 } }, grepInvert: /@touch/ },
    { name: "tablet", use: { ...devices["Desktop Chrome"], viewport: { width: 1180, height: 820 }, hasTouch: true }, grep: /@touch/ },
  ],
  webServer: {
    command: "npx vite --host 127.0.0.1",
    url: "http://127.0.0.1:5174",
    reuseExistingServer: !process.env.CI,
    timeout: 60_000,
  },
});

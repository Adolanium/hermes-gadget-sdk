import { defineConfig } from "@playwright/test";

export default defineConfig({
  testDir: "./test/browser",
  use: {
    baseURL: "http://127.0.0.1:8768",
    headless: true,
    launchOptions: process.env.BROWSER_EXECUTABLE ? {executablePath:process.env.BROWSER_EXECUTABLE} : {},
  },
  webServer: {command:"node scripts/serve.mjs", url:"http://127.0.0.1:8768", reuseExistingServer:!process.env.CI},
});

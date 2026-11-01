import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

import { OUTPUT, bundle } from "../scripts/dashboard-tab.mjs";

test("the dashboard tab's bundle is built from its current source", async () => {
  const current = await readFile(OUTPUT, "utf8");
  assert.equal(current, await bundle(),
    "plugin/dashboard/dist/index.js is out of date: run `npm run build:dashboard-tab` in site/");
});

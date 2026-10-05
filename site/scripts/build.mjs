// Assembles the installer site in one directory: the page, esptool-js from node_modules, images
// from docs/images, and a release's firmware (manifest.json and each board's image, checked
// against the manifest's checksums).
//
//   node scripts/build.mjs [--firmware <release dir>] [--out <dir>]

import { createHash } from "node:crypto";
import { copyFile, cp, mkdir, readFile, rm, writeFile } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";
import { buildDocs } from "./docs.mjs";

const site = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const repo = resolve(site, "..");
const { values } = parseArgs({ options: { firmware: { type: "string" }, out: { type: "string" } } });
const out = resolve(values.out ?? join(site, "_site"));

await rm(out, { recursive: true, force: true });
await cp(join(site, "src"), out, { recursive: true });
await buildDocs(repo, out);

const esptool = join(site, "node_modules", "esptool-js");
await mkdir(join(out, "vendor"));
await copyFile(join(esptool, "bundle.js"), join(out, "vendor", "esptool.js"));
await copyFile(join(esptool, "LICENSE"), join(out, "vendor", "esptool-js-LICENSE.txt"));

await mkdir(join(out, "img"));
for (const name of ["logo.png", "screen-ready.png", "screen-pairing.png"]) {
  await copyFile(join(repo, "docs", "images", name), join(out, "img", name));
}

async function readManifest(dir) {
  try {
    return JSON.parse(await readFile(join(dir, "manifest.json"), "utf8"));
  } catch (error) {
    if (error.code === "ENOENT") return null;
    throw error;
  }
}

const manifest = values.firmware ? await readManifest(resolve(values.firmware)) : null;
if (manifest) {
  const dir = resolve(values.firmware);
  await mkdir(join(out, "firmware"));
  const files = [...manifest.builds.map((build) => build.image), ...(manifest.licenses ? [manifest.licenses] : [])];
  for (const file of files) {
    const data = await readFile(join(dir, file.path));
    const digest = createHash("sha256").update(data).digest("hex");
    if (data.length !== file.size || digest !== file.sha256) {
      throw new Error(`${file.path} doesn't match manifest.json`);
    }
    await writeFile(join(out, "firmware", file.path), data);
  }
  await writeFile(join(out, "firmware", "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);
  console.log(`build: firmware ${manifest.version} for ${manifest.builds.map((b) => b.board).join(", ")}`);
} else {
  console.log("build: no firmware release given; the page says none is published yet");
}
console.log(`build: ${out}`);

import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { test } from "node:test";

import {
  checkChip, embeddedPsram, imageParts, linkParams, loadManifest, megabytes, sha256, sortBuilds, validServer,
} from "../src/lib/boards.js";

const AMOLED = {
  board: "esp32s3-touch-amoled-175", title: "Waveshare ESP32-S3-Touch-AMOLED-1.75", ready_made: true,
  chip: "ESP32-S3", flash_size: "16MB", psram: "octal", settings: { offset: 0x9000, size: 0x6000 },
};
const S3R8 = { name: "ESP32-S3", flashSize: "16MB", features: ["Wi-Fi", "BLE", "Embedded PSRAM 8MB (AP_3v3)"] };

test("ready-made boards come first, then by name", () => {
  const builds = sortBuilds([
    { title: "Zeta breadboard" }, { title: "B board", ready_made: true }, { title: "Alpha breadboard" },
    { title: "A board", ready_made: true },
  ]);
  assert.deepEqual(builds.map((b) => b.title), ["A board", "B board", "Alpha breadboard", "Zeta breadboard"]);
});

test("reads sizes and built-in PSRAM the way esptool reports them", () => {
  assert.equal(megabytes("16MB"), 16);
  assert.equal(megabytes("4 MB"), 4);
  assert.equal(megabytes(undefined), null);
  assert.equal(embeddedPsram(S3R8.features), 8);
  assert.equal(embeddedPsram(["Wi-Fi", "Embedded PSRAM 2MB (AP_3v3)"]), 2);
  assert.equal(embeddedPsram(["Wi-Fi", "BLE"]), 0);
});

test("a matching board passes the check", () => {
  assert.deepEqual(checkChip(AMOLED, S3R8), []);
});

test("another chip or too little flash stops the install", () => {
  const [wrongChip] = checkChip(AMOLED, { ...S3R8, name: "ESP32-C3" });
  assert.equal(wrongChip.level, "error");
  assert.match(wrongChip.message, /has an ESP32-C3, but the firmware for the Waveshare/);
  const [small] = checkChip(AMOLED, { ...S3R8, flashSize: "8MB" });
  assert.deepEqual([small.level, small.message], ["error", "The firmware needs 16 MB of flash, and this board has 8 MB."]);
});

test("the wrong kind of PSRAM, or none, is a warning", () => {
  const [quad] = checkChip(AMOLED, { ...S3R8, features: ["Wi-Fi", "Embedded PSRAM 2MB (AP_3v3)"] });
  assert.equal(quad.level, "warning");
  assert.match(quad.message, /has quad PSRAM \(2 MB\), and the firmware expects octal PSRAM/);
  const [none] = checkChip(AMOLED, { ...S3R8, features: ["Wi-Fi", "BLE"] });
  assert.equal(none.level, "warning");
  assert.match(none.message, /no built-in PSRAM/);
  assert.deepEqual(checkChip({ ...AMOLED, psram: null }, { ...S3R8, features: [] }), []);
});

test("keeping the settings writes around the settings partition", () => {
  assert.deepEqual(imageParts(AMOLED, 0x150000, false), [{ start: 0, end: 0x150000 }]);
  assert.deepEqual(imageParts(AMOLED, 0x150000, true), [{ start: 0, end: 0x9000 }, { start: 0xf000, end: 0x150000 }]);
  assert.deepEqual(imageParts({ ...AMOLED, settings: null }, 0x150000, true), [{ start: 0, end: 0x150000 }]);
});

test("the link from hermes gateway setup fills in the server", () => {
  // As plugin/cli.py's installer_link() encodes it: urllib.parse.quote(server, safe="").
  const link = "#server=ws%3A%2F%2F192.168.1.20%3A8765%2Fgadget";
  assert.deepEqual(linkParams(link), { server: "ws://192.168.1.20:8765/gadget" });
  assert.deepEqual(linkParams("#server=javascript%3Aalert(1)"), { server: "" });
  assert.deepEqual(linkParams(""), { server: "" });
});

test("only ws:// and wss:// addresses with a host are accepted", () => {
  for (const url of ["ws://192.168.1.20:8765/gadget", "wss://hermes.example.com/gadget", "ws://hermes.local:8765"]) {
    assert.ok(validServer(url), url);
  }
  for (const url of ["http://192.168.1.20:8765/gadget", "ws://", "192.168.1.20:8765", "ws://a b/gadget", ""]) {
    assert.ok(!validServer(url), url);
  }
});

test("loads the manifest, and says when there is none", async () => {
  const manifest = { version: "0.1.0", builds: [{ title: "Breadboard" }, { ...AMOLED }] };
  const respond = (status, body) => async () => ({ status, ok: status === 200, json: async () => body });
  const loaded = await loadManifest(respond(200, manifest));
  assert.deepEqual(loaded.builds.map((b) => b.title), [AMOLED.title, "Breadboard"]);
  assert.equal(await loadManifest(respond(404)), null);
  await assert.rejects(loadManifest(respond(500)), /HTTP 500/);
});

test("checks downloads with SHA-256", async () => {
  const bytes = new TextEncoder().encode("firmware");
  assert.equal(await sha256(bytes), createHash("sha256").update(bytes).digest("hex"));
});

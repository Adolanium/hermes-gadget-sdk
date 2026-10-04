// What the release manifest (firmware/esp32/tools/package_release.py) says about each board,
// and whether the chip esptool found can run it.

export const MANIFEST_URL = "firmware/manifest.json";

/** The release manifest, ready-made boards first; null when no release has been published. */
export async function loadManifest(fetchImpl = fetch, url = MANIFEST_URL) {
  const response = await fetchImpl(url, { cache: "no-cache" });
  if (response.status === 404) return null;
  if (!response.ok) throw new Error(`Couldn't load the list of firmware (HTTP ${response.status}).`);
  const manifest = await response.json();
  return { ...manifest, builds: sortBuilds(manifest.builds ?? []) };
}

export function sortBuilds(builds) {
  return [...builds].sort((a, b) => Number(Boolean(b.ready_made)) - Number(Boolean(a.ready_made))
    || a.title.localeCompare(b.title));
}

/** "16MB" -> 16; null when unknown. */
export function megabytes(size) {
  const m = /^(\d+)\s*MB$/i.exec(String(size ?? "").trim());
  return m ? Number(m[1]) : null;
}

/** PSRAM built into the chip in MB, from esptool's features ("Embedded PSRAM 8MB (AP_3v3)"); 0 if none. */
export function embeddedPsram(features) {
  for (const feature of features ?? []) {
    const m = /Embedded PSRAM (\d+)\s*MB/i.exec(feature);
    if (m) return Number(m[1]);
  }
  return 0;
}

// On the ESP32-S3 the size says the kind: the S3R8 has 8 MB of octal PSRAM, the S3R2 2 MB of quad.
const PSRAM_KIND = { 8: "octal", 2: "quad" };

/**
 * What stands between `build` and the chip esptool found ({name, flashSize, features}).
 * Errors stop the install; warnings let the user decide.
 */
export function checkChip(build, chip) {
  if (chip.name !== build.chip) {
    return [{
      level: "error",
      message: `This board has an ${chip.name}, but the firmware for the ${build.title} needs an ${build.chip}. `
        + "Check that you picked the right board.",
    }];
  }
  const problems = [];
  const need = megabytes(build.flash_size);
  const have = megabytes(chip.flashSize);
  if (need && have && have < need) {
    problems.push({ level: "error", message: `The firmware needs ${need} MB of flash, and this board has ${have} MB.` });
  }
  if (build.psram) {
    const size = embeddedPsram(chip.features);
    const kind = PSRAM_KIND[size];
    const effect = "The firmware will start, but the screen may stay dark.";
    if (!size) {
      problems.push({ level: "warning", message: `This chip has no built-in PSRAM, and the firmware expects ${build.psram} PSRAM. ${effect}` });
    } else if (kind && kind !== build.psram) {
      problems.push({
        level: "warning",
        message: `This chip has ${kind} PSRAM (${size} MB), and the firmware expects ${build.psram} PSRAM. ${effect}`,
      });
    }
  }
  return problems;
}

/** The parts of the image to write: all of it, or everything around the settings partition. */
export function imageParts(build, imageLength, keepSettings) {
  const settings = build.settings;
  if (!keepSettings || !settings) return [{ start: 0, end: imageLength }];
  return [
    { start: 0, end: Math.min(settings.offset, imageLength) },
    { start: Math.min(settings.offset + settings.size, imageLength), end: imageLength },
  ].filter((part) => part.end > part.start);
}

/** A device URL the firmware accepts: ws:// or wss://, a host, an optional port and path. */
export function validServer(url) {
  return /^wss?:\/\/[^\s/:?#]+(:\d{1,5})?(\/[^\s]*)?$/i.test(String(url ?? "").trim());
}

/** The installer link's parameters, e.g. `#server=ws%3A%2F%2F192.168.1.20%3A8765%2Fgadget`.
 * They ride in the fragment, which the browser never sends to the web host. */
export function linkParams(hash) {
  const params = new URLSearchParams(String(hash ?? "").replace(/^#/, ""));
  const server = (params.get("server") ?? "").trim();
  return { server: validServer(server) ? server : "" };
}

/** SHA-256 of `bytes` as lowercase hex, to check a download against the manifest. */
export async function sha256(bytes, subtle = globalThis.crypto.subtle) {
  const digest = new Uint8Array(await subtle.digest("SHA-256", bytes));
  return Array.from(digest, (b) => b.toString(16).padStart(2, "0")).join("");
}

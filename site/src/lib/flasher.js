// Writing firmware with esptool-js, Espressif's esptool for the browser.

import { ESPLoader, Transport } from "../vendor/esptool.js";
import { embeddedPsram } from "./boards.js";
import { md5 } from "./md5.js";

/** Put the board in its bootloader and find out what it is. */
export async function connect(port, log) {
  const transport = new Transport(port, false);
  const terminal = { clean() {}, write: log, writeLine: log };
  const loader = new ESPLoader({ transport, baudrate: 921600, romBaudrate: 115200, terminal });
  try {
    await loader.main();
    const features = await loader.chip.getChipFeatures(loader);
    const flashSize = await loader.detectFlashSize();
    const chip = { name: loader.chip.CHIP_NAME, flashSize, features, psram: embeddedPsram(features) };
    return { loader, transport, chip };
  } catch (error) {
    await transport.disconnect().catch(() => {});
    throw error;
  }
}

/** Write `parts` of `image` (see imageParts in boards.js), checking each region's MD5 afterwards. */
export async function write(loader, image, parts, onProgress) {
  const sizes = parts.map((part) => part.end - part.start);
  const total = sizes.reduce((sum, size) => sum + size, 0);
  await loader.writeFlash({
    fileArray: parts.map((part) => ({ data: image.subarray(part.start, part.end), address: part.start })),
    flashMode: "keep",
    flashFreq: "keep",
    flashSize: "keep",
    eraseAll: false,
    compress: true,
    reportProgress: (index, written, size) => {
      const before = sizes.slice(0, index).reduce((sum, s) => sum + s, 0);
      onProgress((before + sizes[index] * (size ? written / size : 1)) / total);
    },
    calculateMD5Hash: md5,
  });
}

/** Restart the board into its new firmware and let go of the port. */
export async function restart(loader, transport) {
  await loader.after("hard_reset");
  await transport.disconnect();
}

// Drives site/src/lib/console.js over stdin and stdout, so tests/test_installer_console.py can put
// the real firmware core on the other end:
//   node test/console-cli.mjs '<settings as JSON>' <result file>
// It waits for the board, stores each setting as the installer does, and writes the status after.

import { writeFile } from "node:fs/promises";
import { Readable, Writable } from "node:stream";

import { DeviceConsole } from "../src/lib/console.js";

const [settingsJson, resultPath] = process.argv.slice(2);
const port = { readable: Readable.toWeb(process.stdin), writable: Writable.toWeb(process.stdout) };
const lines = [];
const board = new DeviceConsole(port, { onLine: (line) => lines.push(line) });

try {
  const before = await board.waitForStatus(10000);
  for (const [key, value] of Object.entries(JSON.parse(settingsJson))) await board.set(key, value);
  const after = await board.status();
  await writeFile(resultPath, JSON.stringify({ before, after, lines }));
  process.exit(0);
} catch (error) {
  await writeFile(resultPath, JSON.stringify({ error: String(error?.message ?? error), lines }));
  process.exit(1);
}

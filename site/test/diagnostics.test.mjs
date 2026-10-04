import assert from "node:assert/strict";
import { test } from "node:test";

import { reportFileName, reportText } from "../src/lib/diagnostics.js";

const WHEN = new Date(2026, 9, 4, 9, 5, 7);
const REPORT = { device_id: "hg-0123456789abcdef", board: "esp32s3-lcd-154", firmware: "0.1.1" };

test("the report reads like the one hermes-gadget diag saves", () => {
  const text = reportText({ report: REPORT, log: ["I (312) hg.main: start", "W (318) hg.wifi: no Wi-Fi"] }, WHEN);
  assert.equal(text, "Hermes Gadget diagnostics from the browser installer, 2026-10-04 09:05:07\n\n"
    + `== report ==\n${JSON.stringify(REPORT, null, 2)}\n\n`
    + "== recent log ==\nI (312) hg.main: start\nW (318) hg.wifi: no Wi-Fi\n");
});

test("the file is named after the device and the time, with nothing a file system rejects", () => {
  assert.equal(reportFileName(REPORT, WHEN), "hermes-gadget-diag-hg-0123456789abcdef-20261004-090507.txt");
  assert.equal(reportFileName({ device_id: "a/b:c" }, WHEN), "hermes-gadget-diag-a_b_c-20261004-090507.txt");
  assert.equal(reportFileName({}, WHEN), "hermes-gadget-diag-device-20261004-090507.txt");
});

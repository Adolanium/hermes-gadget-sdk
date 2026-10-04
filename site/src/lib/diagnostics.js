// A board's diagnostics report as a file, in the format `hermes-gadget diag` saves
// (cmd_diag in python/hermes_gadget/cli.py), so a bug report looks the same either way.

const pad = (n) => String(n).padStart(2, "0");
const day = (t) => `${t.getFullYear()}-${pad(t.getMonth() + 1)}-${pad(t.getDate())}`;
const clock = (t) => `${pad(t.getHours())}:${pad(t.getMinutes())}:${pad(t.getSeconds())}`;

/** The file's text: where and when it was taken, the report, then the board's recent log. */
export function reportText({ report, log }, when = new Date()) {
  return `Hermes Gadget diagnostics from the browser installer, ${day(when)} ${clock(when)}\n\n`
    + `== report ==\n${JSON.stringify(report, null, 2)}\n\n== recent log ==\n${log.join("\n")}\n`;
}

/** hermes-gadget-diag-<device>-<YYYYMMDD-HHMMSS>.txt */
export function reportFileName(report, when = new Date()) {
  const device = String(report?.device_id || "device").replace(/[^A-Za-z0-9_.-]/g, "_");
  const stamp = `${day(when).replaceAll("-", "")}-${clock(when).replaceAll(":", "")}`;
  return `hermes-gadget-diag-${device}-${stamp}.txt`;
}

import assert from "node:assert/strict";
import { test } from "node:test";

import { ConsoleError, DeviceConsole, consoleSafe, quoteArg, redact } from "../src/lib/console.js";

/**
 * A board's console over a serial line: boot noise first, the first command swallowed (ESP-IDF
 * probes the terminal), then an echo after the prompt and App::console's reply, in small chunks.
 */
function fakeBoard(app, { swallow = 1, chunk = 7 } = {}) {
  const encoder = new TextEncoder();
  let emit;
  let unplugged = false;
  const readable = new ReadableStream({ start(controller) { emit = controller; } });
  const send = (text) => {
    if (unplugged) return;
    const bytes = encoder.encode(text);
    for (let i = 0; i < bytes.length; i += chunk) emit.enqueue(bytes.slice(i, i + chunk));
  };
  const received = [];
  let pending = "";
  const writable = new WritableStream({
    write(bytes) {
      pending += new TextDecoder().decode(bytes);
      let end;
      while ((end = pending.indexOf("\n")) >= 0) {
        const line = pending.slice(0, end);
        pending = pending.slice(end + 1);
        received.push(line);
        if (swallow > 0) {
          swallow -= 1;
          continue;
        }
        const reply = app(line);
        send(`gadget>${line}\n${reply === undefined ? "" : `${reply}\n`}`);
      }
    },
  });
  send("I (312) hg.main: Hermes Gadget 0.1.0 on esp32s3-lcd-154\n@ this is not a reply\n");
  const end = () => {
    unplugged = true;
    emit.close();
  };
  return { port: { readable, writable }, received, end };
}

const STATUS = { device_id: "hg-0123456789abcdef", phase: "no_network", paired: false };

test("quoting gets spaces, quotes and backslashes through, and secrets stay out of the log", () => {
  assert.equal(quoteArg('my "home" \\ net'), '"my \\"home\\" \\\\ net"');
  assert.equal(quoteArg(""), '""');
  assert.ok(consoleSafe("Hunter2 !@#$%^&*()_+-=[]{};':,./<>?"));
  assert.ok(!consoleSafe("Café"));
  assert.ok(!consoleSafe("tab\there"));
  assert.equal(redact('gadget>set wifi_pass "secret"'), "gadget>set wifi_pass (hidden)");
  assert.equal(redact('gadget>set token "abc"'), "gadget>set token (hidden)");
  assert.equal(redact('gadget>set wifi_ssid "Home"'), 'gadget>set wifi_ssid "Home"');
});

test("waits out the start-up, then stores each setting", async () => {
  const stored = {};
  const board = fakeBoard((line) => {
    if (line === "status") return `@status ${JSON.stringify(STATUS)}`;
    const m = /^set (\w+) "(.*)"$/.exec(line);
    if (!m) return "@error unknown command (try: help)";
    stored[m[1]] = m[2];
    return `@ok ${m[1]}`;
  });
  const seen = [];
  const device = new DeviceConsole(board.port, { onLine: (line) => seen.push(line) });
  assert.deepEqual(await device.waitForStatus(5000), STATUS);
  await device.set("wifi_ssid", "Home");
  await device.set("server", "ws://192.168.1.20:8765/gadget");
  assert.deepEqual(stored, { wifi_ssid: "Home", server: "ws://192.168.1.20:8765/gadget" });
  assert.deepEqual(board.received.slice(0, 2), ["status", "status"]); // the first one was swallowed
  assert.ok(seen.includes("I (312) hg.main: Hermes Gadget 0.1.0 on esp32s3-lcd-154"));
  await device.close();
});

test("a refused setting says why", async () => {
  const board = fakeBoard((line) => (line === "status" ? "@status {}" : "@error unknown key"), { swallow: 0 });
  const device = new DeviceConsole(board.port);
  await assert.rejects(device.set("colour", "red"), (error) => error instanceof ConsoleError
    && error.message === "The board refused colour: unknown key.");
  await device.close();
});

test("silence is a timeout, not a hang, and stray @ lines aren't replies", async () => {
  const board = fakeBoard(() => undefined, { swallow: 0 });
  const device = new DeviceConsole(board.port);
  assert.equal(await device.command("status", 100, (line) => line.startsWith("@status ")), null);
  await assert.rejects(device.waitForStatus(200), /didn't answer on this port/);
  await device.close();
});

test("a late reply to an earlier command doesn't answer the next one", async () => {
  let calls = 0;
  const board = fakeBoard((line) => {
    calls += 1;
    if (calls === 1) return undefined; // the first set goes unanswered...
    return calls === 2 ? "@ok wifi_ssid\n@ok wifi_pass" : undefined; // ...and its reply turns up late
  }, { swallow: 0 });
  const device = new DeviceConsole(board.port);
  await assert.rejects(device.set("wifi_ssid", "Home"), /didn't confirm wifi_ssid/);
  await device.set("wifi_pass", "secret"); // takes "@ok wifi_pass", skipping the late "@ok wifi_ssid"
  await device.close();
});

test("an unplugged board ends the wait at once", async () => {
  const board = fakeBoard(() => undefined, { swallow: 0 });
  const device = new DeviceConsole(board.port);
  const waiting = device.command("status", 10000, (line) => line.startsWith("@status "));
  await new Promise((resolve) => setTimeout(resolve, 20));
  const started = Date.now();
  board.end();
  await assert.rejects(waiting, /connection to the board closed/);
  assert.ok(Date.now() - started < 1000);
  await assert.rejects(device.command("status"), /connection to the board closed/);
});

test("a line too long for the console is refused before sending", async () => {
  const board = fakeBoard(() => "@ok x", { swallow: 0 });
  const device = new DeviceConsole(board.port);
  await assert.rejects(device.set("server", `ws://${"a".repeat(260)}`), /too long/);
  assert.deepEqual(board.received, []);
  await device.close();
});

// The installer page: choose a board, write its firmware, give it Wi-Fi and the Hermes address,
// then watch it pair. Everything happens between this page and the board, over Web Serial.

import { checkChip, imageParts, linkParams, loadManifest, sha256, validServer } from "./lib/boards.js";
import { DeviceConsole, consoleSafe, redact } from "./lib/console.js";
import { openConsole, reopen } from "./lib/serial.js";

const REPO = "https://github.com/Adolanium/hermes-gadget-sdk";
const STEPS = ["step-board", "step-install", "step-wifi", "step-pair"];
const POLL_MS = 1500;
const WIFI_PATIENCE_MS = 25000;
const HERMES_PATIENCE_MS = 20000;

const $ = (id) => document.getElementById(id);
const state = { manifest: null, build: null, port: null, console: null, writing: false, poll: null };

// esptool-js is loaded only when a board is flashed, so the page works without it.
const flasher = () => import("./lib/flasher.js");

// ---- page plumbing --------------------------------------------------------------------------

function log(text) {
  const pre = $("log");
  const lines = `${pre.textContent}${String(text).replace(/\s+$/, "")}\n`.split("\n");
  pre.textContent = lines.slice(-400).join("\n");
  pre.scrollTop = pre.scrollHeight;
}

function show(id, visible = true) {
  $(id).hidden = !visible;
}

function setNotice(id, text) {
  const el = $(id);
  el.textContent = "";
  if (text) {
    const p = document.createElement("p");
    p.textContent = text;
    el.append(p);
  }
  el.hidden = !text;
}

function goTo(step) {
  const index = STEPS.indexOf(step);
  STEPS.forEach((id, i) => {
    $(id).dataset.state = i < index ? "done" : i === index ? "active" : "locked";
  });
  $(step).querySelector("h2").focus({ preventScroll: true });
  $(step).scrollIntoView({ behavior: matchMedia("(prefers-reduced-motion: reduce)").matches ? "auto" : "smooth", block: "start" });
}

function friendly(error) {
  const message = String(error?.message ?? error);
  if (/Failed to open serial port/i.test(message)) {
    return "The port is in use by another program, such as a serial monitor. Close it and try again.";
  }
  if (/Failed to connect|Couldn't sync|Invalid head of packet|Timeout/i.test(message)) {
    return "The board didn't answer. Hold its BOOT button, press and release RESET, let go of BOOT, and try again.";
  }
  if (/device has been lost|The device has been disconnected|NetworkError/i.test(message)) {
    return "The board was disconnected. Plug it back in and try again.";
  }
  return message;
}

async function choosePort() {
  try {
    return await navigator.serial.requestPort();
  } catch (error) {
    if (error?.name === "NotFoundError") return null; // the user closed the chooser
    throw error;
  }
}

// ---- step 1: the board ----------------------------------------------------------------------

function renderBoards(manifest) {
  const container = $("boards");
  container.textContent = "";
  for (const build of manifest.builds) {
    const card = document.createElement("label");
    card.className = "board";
    const input = document.createElement("input");
    input.type = "radio";
    input.name = "board";
    input.value = build.board;
    input.addEventListener("change", () => {
      state.build = build;
      $("board-next").disabled = false;
    });
    const title = document.createElement("span");
    title.className = "board-title";
    title.textContent = build.title;
    const summary = document.createElement("span");
    summary.className = "board-summary";
    summary.textContent = build.summary;
    card.append(input, title);
    if (build.ready_made) {
      const badge = document.createElement("span");
      badge.className = "badge";
      badge.textContent = "Nothing to wire";
      card.append(badge);
    }
    card.append(summary);
    if (build.docs) {
      const docs = document.createElement("a");
      docs.className = "board-docs";
      docs.href = `${REPO}/blob/main/${build.docs}`;
      docs.textContent = "Pins and details";
      docs.target = "_blank";
      docs.rel = "noopener";
      card.append(docs);
    }
    container.append(card);
  }
}

$("board-next").addEventListener("click", () => {
  $("board-summary").textContent = state.build.title;
  show("uart-hint", !state.build.ready_made);
  goTo("step-install");
});

$("skip-install").addEventListener("click", () => {
  $("board-summary").textContent = "Already running Hermes Gadget";
  $("install-summary").textContent = "Skipped";
  goTo("step-wifi");
});

// ---- step 2: the firmware -------------------------------------------------------------------

function progress(text, fraction) {
  show("install-progress");
  $("install-status").textContent = text;
  const bar = $("install-bar");
  const percent = fraction == null ? 0 : Math.round(fraction * 100);
  bar.firstElementChild.style.width = `${percent}%`;
  bar.setAttribute("aria-valuenow", String(percent));
  bar.classList.toggle("indeterminate", fraction == null);
}

async function download(build) {
  const response = await fetch(`firmware/${build.image.path}`, { cache: "no-cache" });
  if (!response.ok) throw new Error(`Couldn't download the firmware (HTTP ${response.status}).`);
  const image = new Uint8Array(await response.arrayBuffer());
  if (image.length !== build.image.size || (await sha256(image)) !== build.image.sha256) {
    throw new Error("The downloaded firmware doesn't match its checksum. Reload the page and try again.");
  }
  return image;
}

function askToContinue(warnings) {
  $("install-warning-text").textContent = warnings.map((w) => w.message).join(" ");
  show("install-warning");
  return new Promise((resolve) => {
    const answer = (yes) => {
      show("install-warning", false);
      $("install-anyway").onclick = $("install-cancel").onclick = null;
      resolve(yes);
    };
    $("install-anyway").onclick = () => answer(true);
    $("install-cancel").onclick = () => answer(false);
  });
}

async function install() {
  const build = state.build;
  setNotice("install-error", "");
  const port = await choosePort();
  if (!port) return;
  $("install-actions").hidden = true;
  let session = null;
  try {
    progress("Downloading the firmware...", null);
    const image = await download(build);
    const { connect, write, restart } = await flasher();
    progress("Connecting to the board...", null);
    session = await connect(port, log);
    const { chip } = session;
    log(`${chip.name}, ${chip.flashSize ?? "unknown"} flash, features: ${chip.features.join(", ")}`);
    const problems = checkChip(build, chip);
    const errors = problems.filter((p) => p.level === "error");
    if (errors.length) throw new Error(errors.map((p) => p.message).join(" "));
    const warnings = problems.filter((p) => p.level === "warning");
    if (warnings.length && !(await askToContinue(warnings))) {
      await session.transport.disconnect();
      show("install-progress", false);
      return;
    }

    const found = `Found an ${chip.name} with ${chip.flashSize ?? "unknown"} of flash`
      + (chip.psram ? ` and ${chip.psram} MB of PSRAM.` : ".");
    state.writing = true;
    const parts = imageParts(build, image.length, $("keep-settings").checked);
    await write(session.loader, image, parts, (fraction) => {
      progress(`${found} Writing the firmware... ${Math.round(fraction * 100)}%`, fraction);
    });
    state.writing = false;
    progress("Installed. Restarting the board...", 1);
    await restart(session.loader, session.transport);
    session = null;

    state.port = await reopen(port);
    if (!state.port) log("The board didn't come back on the same port; the next step asks for it again.");
    progress(`Installed Hermes Gadget ${state.manifest.version}.`, 1);
    $("install-summary").textContent = `Hermes Gadget ${state.manifest.version} installed`;
    goTo("step-wifi");
  } catch (error) {
    log(`Install failed: ${error?.message ?? error}`);
    show("install-progress", false);
    setNotice("install-error", friendly(error));
    await session?.transport.disconnect().catch(() => {});
  } finally {
    state.writing = false;
    $("install-actions").hidden = false;
  }
}

$("install").addEventListener("click", install);
$("install-back").addEventListener("click", () => goTo("step-board"));

// ---- step 3: Wi-Fi and Hermes ---------------------------------------------------------------

function formValues() {
  const value = (id) => $(id).value.trim();
  return {
    wifi_ssid: value("ssid"),
    wifi_pass: $("password").value,
    server: value("server"),
    name: value("name"),
    token: value("token"),
  };
}

// Each setting's form field, and its name in messages.
const FIELDS = {
  wifi_ssid: ["ssid", "Wi-Fi network name"],
  wifi_pass: ["password", "Wi-Fi password"],
  name: ["name", "device name"],
  token: ["token", "access token"],
};

function formProblem(values) {
  if (!values.wifi_ssid) return ["ssid", "Enter the name of your Wi-Fi network."];
  if (!validServer(values.server)) {
    return ["server", "Enter the Hermes address, starting with ws:// or wss://, for example ws://192.168.1.20:8765/gadget."];
  }
  for (const [key, [field, label]] of Object.entries(FIELDS)) {
    if (!consoleSafe(values[key])) {
      return [field, `The ${label} has characters the board's console can't receive. Only plain ASCII letters, `
        + "digits and symbols work here."];
    }
  }
  return null;
}

async function connectConsole() {
  if (!state.port) {
    const port = await choosePort();
    if (!port) return false;
    state.port = await openConsole(port);
  }
  state.console ??= new DeviceConsole(state.port, { onLine: (line) => log(redact(line)) });
  return true;
}

async function saveSettings(event) {
  event.preventDefault();
  setNotice("wifi-error", "");
  const values = formValues();
  const problem = formProblem(values);
  if (problem) {
    setNotice("wifi-error", problem[1]);
    $(problem[0]).closest("details")?.setAttribute("open", "");
    $(problem[0]).focus();
    return;
  }
  const status = $("wifi-status");
  const button = $("wifi-save");
  button.disabled = true;
  try {
    if (!(await connectConsole())) return;
    status.hidden = false;
    status.textContent = "Waiting for the board to answer...";
    const before = await state.console.waitForStatus();
    log(`Board ${before.device_id} (${before.board}), firmware ${before.firmware}`);
    const settings = [["wifi_ssid", values.wifi_ssid], ["wifi_pass", values.wifi_pass], ["server", values.server]];
    if (values.token) settings.push(["token", values.token]);
    if (values.name) settings.push(["name", values.name]);
    for (const [key, value] of settings) {
      status.textContent = `Saving ${key.replace("_", " ")}...`;
      await state.console.set(key, value);
    }
    status.hidden = true;
    $("wifi-summary").textContent = `${values.wifi_ssid} · ${values.server}`;
    goTo("step-pair");
    watchPairing(values);
  } catch (error) {
    status.hidden = true;
    log(`Saving the settings failed: ${error?.message ?? error}`);
    let text = friendly(error);
    if (/didn't answer on this port/.test(text)) {
      text += " If it has two USB ports, use the one marked UART. If it was just installed, press its RESET button.";
    }
    setNotice("wifi-error", text);
    await disconnect();
  } finally {
    button.disabled = false;
  }
}

$("wifi-form").addEventListener("submit", saveSettings);
$("show-password").addEventListener("click", (event) => {
  const visible = $("password").type === "password";
  $("password").type = visible ? "text" : "password";
  event.currentTarget.textContent = visible ? "Hide" : "Show";
  event.currentTarget.setAttribute("aria-pressed", String(visible));
});

// ---- step 4: pairing ------------------------------------------------------------------------

function check(id, stateName, label) {
  const item = $(id);
  item.dataset.state = stateName;
  if (label) item.lastElementChild.textContent = label;
}

function watchPairing(values) {
  const started = Date.now();
  let onWifiSince = null;
  let busy = false;
  const tick = async () => {
    if (busy || !state.console) return;
    busy = true;
    try {
      render(await state.console.status());
    } catch (error) {
      setNotice("pair-hint", `Lost the connection to the board: ${friendly(error)}`);
      stopWatching();
    } finally {
      busy = false;
    }
  };
  const render = (s) => {
    const reached = s.phase === "online" || Boolean(s.pairing_code);
    if (s.network && onWifiSince == null) onWifiSince = Date.now();
    check("check-wifi", s.network ? "done" : "active", s.network ? `On ${values.wifi_ssid}` : `Joining ${values.wifi_ssid}`);
    check("check-hermes", reached ? "done" : s.network ? "active" : "pending", reached ? "Connected to Hermes" : "Reaching Hermes");
    const paired = s.paired && s.phase === "online";
    check("check-paired", paired ? "done" : reached ? "active" : "pending", paired ? "Paired" : "Waiting for your approval");

    show("pairing", Boolean(s.pairing_code) && !paired);
    if (s.pairing_code) {
      $("pair-code").textContent = s.pairing_code;
      $("approve-command").textContent = `hermes pairing approve gadget ${s.pairing_code}`;
    }
    let hint = "";
    if (s.error) {
      hint = `The board reports: ${s.error}`;
    } else if (!s.network && Date.now() - started > WIFI_PATIENCE_MS) {
      hint = `The board hasn't joined ${values.wifi_ssid} yet. Check the password, and that it's a 2.4 GHz network.`;
    } else if (s.network && !reached && Date.now() - onWifiSince > HERMES_PATIENCE_MS) {
      hint = `The board is on Wi-Fi but can't reach Hermes at ${values.server}. Check the address with `
        + "hermes gadget info, that the gateway is running, and that its firewall allows port 8765.";
    }
    setNotice("pair-hint", hint);
    if (paired) {
      show("pair-done");
      stopWatching();
    }
  };
  check("check-wifi", "active");
  stopWatching();
  state.poll = setInterval(tick, POLL_MS);
  tick();
}

function stopWatching() {
  clearInterval(state.poll);
  state.poll = null;
}

async function disconnect() {
  stopWatching();
  const { console: session, port } = state;
  state.console = null;
  state.port = null;
  await session?.close().catch(() => {});
  await port?.close().catch(() => {});
}

$("disconnect").addEventListener("click", async () => {
  await disconnect();
  setNotice("pair-hint", "Disconnected. You can unplug the board; it keeps its settings.");
});
$("another").addEventListener("click", async () => {
  await disconnect();
  location.reload();
});

document.addEventListener("click", async (event) => {
  const button = event.target.closest("[data-copy]");
  if (!button) return;
  await navigator.clipboard.writeText(button.dataset.copy);
  const label = button.textContent;
  button.textContent = "Copied";
  setTimeout(() => { button.textContent = label; }, 1500);
});

window.addEventListener("beforeunload", (event) => {
  if (state.writing) event.preventDefault();
});

// ---- start ----------------------------------------------------------------------------------

async function start() {
  const { server } = linkParams(location.hash);
  if (server) $("server").value = server;

  if (!("serial" in navigator)) {
    show("unsupported");
    for (const id of ["board-next", "skip-install", "install", "wifi-save"]) $(id).disabled = true;
  } else {
    navigator.serial.addEventListener("disconnect", (event) => {
      if (event.target === state.port && !state.writing) {
        log("The board was unplugged.");
        disconnect();
      }
    });
  }

  try {
    state.manifest = await loadManifest();
  } catch (error) {
    log(error.message);
  }
  if (state.manifest?.builds.length) {
    renderBoards(state.manifest);
    $("firmware-version").textContent = `Firmware ${state.manifest.version}.`;
  } else {
    show("no-firmware");
  }
}

start();

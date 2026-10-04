// Opening the board's serial port for its console, and finding it again after a restart.

const CONSOLE_BAUD = 115200;

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function sameDevice(a, b) {
  return a.usbVendorId === b.usbVendorId && a.usbProductId === b.usbProductId;
}

/** Open `port` for the console, with the reset and boot-mode lines released. */
export async function openConsole(port) {
  await port.open({ baudRate: CONSOLE_BAUD });
  await port.setSignals({ dataTerminalReady: false, requestToSend: false }).catch(() => {});
  return port;
}

/**
 * The port again after the board restarted. A board on its own USB (USB Serial/JTAG) drops off the
 * bus while it restarts, so wait for it to come back. The browser keeps the permission, so this needs
 * no new prompt. Null if it doesn't return in time.
 */
export async function reopen(port, timeoutMs = 15000) {
  const info = port.getInfo();
  const deadline = Date.now() + timeoutMs;
  await sleep(1000);
  while (Date.now() < deadline) {
    const ports = await navigator.serial.getPorts();
    const candidates = [port, ...ports.filter((p) => p !== port && sameDevice(p.getInfo(), info))];
    for (const candidate of candidates.filter((p) => ports.includes(p))) {
      try {
        return await openConsole(candidate);
      } catch {
        // still coming back, or busy for a moment
      }
    }
    await sleep(500);
  }
  return null;
}

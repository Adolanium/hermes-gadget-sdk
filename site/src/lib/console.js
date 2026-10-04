// The firmware's serial console as the installer uses it: one command per line, answered by a
// line that starts with "@" (App::console in firmware/core/src/app.cpp).
//
// ESP-IDF's console sits in between. It echoes what it reads, drops anything but printable ASCII,
// and splits arguments at spaces, honouring double quotes and backslash escapes, before the
// firmware joins them back into one line. quoteArg() gets any printable value through intact.

const encoder = new TextEncoder();

// ESP-IDF reads at most max_cmdline_length (256) bytes per line, the terminator included.
const MAX_LINE = 255;

export class ConsoleError extends Error {}

const isStatus = (line) => line.startsWith("@status ");

/** `value` as a single console argument: in double quotes, with backslashes and quotes escaped. */
export function quoteArg(value) {
  return `"${String(value).replace(/[\\"]/g, "\\$&")}"`;
}

/** True when every character survives the console: printable ASCII only. */
export function consoleSafe(value) {
  return /^[\x20-\x7e]*$/.test(String(value));
}

/** A console line with any setting's value hidden, for showing in the page's log. */
export function redact(line) {
  return line.replace(/(set\s+(?:wifi_pass|token)\s+).*/i, "$1(hidden)");
}

export class DeviceConsole {
  /**
   * @param port an open port: {readable: ReadableStream<Uint8Array>, writable: WritableStream<Uint8Array>}
   * @param options.onLine sees every line the device prints
   */
  constructor(port, { onLine } = {}) {
    this._reader = port.readable.getReader();
    this._writer = port.writable.getWriter();
    this._decoder = new TextDecoder();
    this._pending = "";
    this._replies = [];
    this._wake = null;
    this._closed = false;
    this._failure = null;
    this._onLine = onLine;
    this._capture = null;
    this._busy = Promise.resolve();
    this._pump = this._read();
  }

  async _read() {
    try {
      for (;;) {
        const { value, done } = await this._reader.read();
        if (done) break;
        this._pending += this._decoder.decode(value, { stream: true });
        let end;
        while ((end = this._pending.indexOf("\n")) >= 0) {
          const line = this._pending.slice(0, end).replace(/\r$/, "");
          this._pending = this._pending.slice(end + 1);
          this._onLine?.(line);
          this._capture?.push(line);
          if (line.startsWith("@")) {
            this._replies.push(line);
            this._notify();
          }
        }
      }
    } catch (error) {
      if (!this._closed) this._failure = error;
    } finally {
      this._closed = true;
      this._notify();
    }
  }

  _notify() {
    const wake = this._wake;
    this._wake = null;
    wake?.();
  }

  _lost(error) {
    const reason = error ?? this._failure;
    return new ConsoleError(reason ? `The connection to the board closed (${reason.message}).`
      : "The connection to the board closed.");
  }

  async _reply(accept, deadline) {
    for (;;) {
      const index = this._replies.findIndex(accept);
      if (index >= 0) return this._replies.splice(0, index + 1).pop();
      this._replies.length = 0; // lines this command doesn't answer to
      if (this._closed) throw this._lost();
      const left = deadline - Date.now();
      if (left <= 0) return null;
      await new Promise((resolve) => {
        this._wake = resolve;
        setTimeout(resolve, left);
      });
    }
  }

  /**
   * Send a command and return the first "@" line `accept` takes as its reply, or null if none
   * arrives in time. Other "@" lines, such as a late reply to an earlier command, are skipped.
   */
  async command(line, timeoutMs = 3000, accept = (reply) => reply.startsWith("@")) {
    return (await this._exchange(line, timeoutMs, accept)).reply;
  }

  /** Commands run one at a time, so two callers (the pairing watch and a report) never share replies.
   * With `capture`, every line the board prints until the reply comes back too. */
  _exchange(line, timeoutMs, accept, capture = false) {
    const run = this._busy.then(() => this._send(line, timeoutMs, accept, capture));
    this._busy = run.catch(() => {});
    return run;
  }

  async _send(line, timeoutMs, accept, capture) {
    if (line.length > MAX_LINE) throw new ConsoleError("That value is too long for the board's console.");
    if (this._closed) throw this._lost();
    this._replies.length = 0;
    this._capture = capture ? [] : null;
    try {
      await this._writer.write(encoder.encode(`${line}\n`));
      return { reply: await this._reply(accept, Date.now() + timeoutMs), lines: this._capture ?? [] };
    } catch (error) {
      throw error instanceof ConsoleError ? error : this._lost(error);
    } finally {
      this._capture = null;
    }
  }

  /** The device's status, retried until the console answers: a board that just restarted is still
   * starting up, and ESP-IDF swallows the first input while it probes the terminal. */
  async waitForStatus(timeoutMs = 20000) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      const wait = Math.max(1, Math.min(1500, deadline - Date.now()));
      const reply = await this.command("status", wait, isStatus);
      if (reply) return JSON.parse(reply.slice("@status ".length));
    }
    throw new ConsoleError("The board didn't answer on this port.");
  }

  async status() {
    const reply = await this.command("status", 3000, isStatus);
    if (!reply) throw new ConsoleError("The board didn't report its status.");
    return JSON.parse(reply.slice("@status ".length));
  }

  /** The board's diagnostics report and recent log, read the way `hermes-gadget diag` reads them. */
  async diagnostics() {
    const { reply } = await this._exchange("diag", 8000, (line) => line.startsWith("@diag ") || line.startsWith("@error "));
    if (!reply?.startsWith("@diag ")) {
      throw new ConsoleError(reply ? `The board refused the report: ${reply.slice("@error ".length)}.`
        : "The board didn't send its report.");
    }
    const report = JSON.parse(reply.slice("@diag ".length));
    const { lines } = await this._exchange("diag log", 8000,
      (line) => line === "@log end" || line.startsWith("@error "), true);
    return { report, log: lines.filter((line) => line !== "@log end" && !line.startsWith("gadget>")) };
  }

  /** Store a setting in the device's flash. An empty value erases it. */
  async set(key, value) {
    const ok = `@ok ${key}`;
    const reply = await this.command(`set ${key} ${quoteArg(value)}`, 3000,
      (line) => line === ok || line.startsWith("@error "));
    if (reply === ok) return;
    throw new ConsoleError(reply ? `The board refused ${key}: ${reply.slice("@error ".length)}.`
      : `The board didn't confirm ${key}.`);
  }

  async close() {
    this._closed = true;
    await this._reader.cancel().catch(() => {});
    await this._pump;
    this._reader.releaseLock();
    this._writer.releaseLock();
  }
}

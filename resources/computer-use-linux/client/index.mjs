import { spawn } from 'child_process';
import { existsSync } from 'fs';

/**
 * Client for the LobsterAI Computer Use Linux helper.
 *
 * The helper is a native binary speaking newline-delimited JSON-RPC 2.0 over
 * stdio (see resources/computer-use-linux). This module is the only place that
 * knows the wire format; the MCP server above it sees the same method names it
 * sees on macOS and Windows.
 *
 * Design notes
 *  - One long-lived child process, lazily spawned on the first call and shared
 *    by every request. Spawning per call would cost an AT-SPI bus connection
 *    each time, and the helper keeps a state_id -> element cache that only makes
 *    sense across the get_window_state / click pair.
 *  - Requests are serialised through a promise chain. The helper handles one
 *    request at a time anyway, and AT-SPI calls are synchronous D-Bus round
 *    trips; interleaving would only add races.
 *  - A crashed helper is detected and respawned on the next call, because a
 *    broken D-Bus session or a killed process should not poison the whole turn.
 */

const DEFAULT_TIMEOUT_MS = 30000;

/** Methods the helper exposes, mirroring the macOS helper's surface. */
const KNOWN_METHODS = [
  'activate_window',
  'check_permissions',
  'click',
  'close',
  'describe_environment',
  'drag',
  'get_window',
  'get_window_state',
  'launch_app',
  'list_apps',
  'list_windows',
  'perform_secondary_action',
  'press_key',
  'request_input_access',
  'request_permissions',
  'resolve_app',
  'scroll',
  'set_accessibility',
  'set_value',
  'shutdown',
  'type_text',
];

/**
 * The helper wraps every reply as an MCP-style envelope
 * `{ content: [{ type: "text", text: "<JSON or plain message>" }] }`. The
 * bridge above this client expects the bare result object (or plain string)
 * that macOS and Windows helpers return directly, so this layer — the only one
 * that knows the wire format — unwraps the envelope here. A reply that is
 * already bare passes through unchanged; a text part that is not JSON falls
 * back to its raw string.
 */
function unwrapResult(result) {
  if (
    result && typeof result === 'object' && !Array.isArray(result)
    && Array.isArray(result.content)
  ) {
    const texts = result.content
      .filter((item) => item && item.type === 'text' && typeof item.text === 'string')
      .map((item) => item.text);
    if (texts.length > 0) {
      const joined = texts.join('\n');
      try {
        return JSON.parse(joined);
      } catch {
        return joined;
      }
    }
  }
  return result;
}

export class ComputerUseClient {
  /** @param {{ helperPath: string, timeoutMs?: number, env?: Object }} options */
  constructor(options) {
    this.options = options;
    this.child = null;
    this.nextId = 1;
    this.pending = new Map();
    this.stdoutBuffer = '';
    this.stderrTail = [];
    this.queue = Promise.resolve();
    this.closed = false;
  }

  // ------------------------------------------------------------------ lifecycle

  ensureChild() {
    if (this.child && this.child.exitCode === null && !this.child.killed) {
      return this.child;
    }
    if (!existsSync(this.options.helperPath)) {
      throw new Error(
        `Computer Use helper not found at ${this.options.helperPath}. `
        + 'Reinstall the Computer Use runtime.',
      );
    }

    const child = spawn(this.options.helperPath, [], {
      stdio: ['pipe', 'pipe', 'pipe'],
      env: { ...process.env, ...this.options.env },
    });
    this.child = child;
    this.stdoutBuffer = '';
    this.stderrTail = [];

    child.stdout?.setEncoding('utf8');
    child.stdout?.on('data', chunk => this.consumeStdout(chunk));

    child.stderr?.setEncoding('utf8');
    child.stderr?.on('data', chunk => {
      // Keep a short tail for error messages; the helper logs diagnostics here
      // and they explain most failures far better than "request failed".
      for (const line of String(chunk).split('\n')) {
        if (line.trim()) {
          this.stderrTail.push(line);
          if (this.stderrTail.length > 20) {
            this.stderrTail.shift();
          }
        }
      }
      console.error('[ComputerUseLinux]', String(chunk).trimEnd());
    });

    const failAll = reason => {
      for (const [id, entry] of this.pending) {
        clearTimeout(entry.timer);
        entry.reject(new Error(`${entry.method}: helper exited — ${reason}`));
        this.pending.delete(id);
      }
    };
    child.on('error', error => failAll(error.message));
    child.on('exit', (code, signal) => {
      this.child = null;
      failAll(`exit code ${String(code)} signal ${String(signal)}`
        + (this.stderrTail.length ? `: ${this.stderrTail.slice(-3).join(' | ')}` : ''));
    });

    return child;
  }

  consumeStdout(chunk) {
    this.stdoutBuffer += chunk;
    let newline = this.stdoutBuffer.indexOf('\n');
    while (newline !== -1) {
      const line = this.stdoutBuffer.slice(0, newline).trim();
      this.stdoutBuffer = this.stdoutBuffer.slice(newline + 1);
      if (line) {
        this.dispatch(line);
      }
      newline = this.stdoutBuffer.indexOf('\n');
    }
  }

  dispatch(line) {
    let message;
    try {
      message = JSON.parse(line);
    } catch {
      console.error('[ComputerUseLinux] unparsable helper output:', line.slice(0, 200));
      return;
    }
    const id = message?.id;
    if (typeof id !== 'number') {
      return;
    }
    const entry = this.pending.get(id);
    if (!entry) {
      return;
    }
    this.pending.delete(id);
    clearTimeout(entry.timer);

    if (message.error) {
      const detail = message.error.message || JSON.stringify(message.error);
      const tail = this.stderrTail.length ? ` (${this.stderrTail.slice(-1)[0]})` : '';
      entry.reject(new Error(`${entry.method}: ${detail}${tail}`));
      return;
    }
    entry.resolve(unwrapResult(message.result ?? null));
  }

  /**
   * Sends one request. Public so tests can hit a method that has no typed
   * wrapper yet, but the typed methods below are what the MCP server calls.
   */
  request(method, params) {
    if (this.closed) {
      return Promise.reject(new Error('Computer Use client is closed'));
    }
    const run = () => this.send(method, params);
    const result = this.queue.then(run, run);
    // Keep the chain alive even when a call rejects, so one failure does not
    // wedge every later request.
    this.queue = result.then(() => undefined, () => undefined);
    return result;
  }

  send(method, params) {
    const child = this.ensureChild();
    const id = this.nextId++;
    const timeoutMs = this.options.timeoutMs ?? DEFAULT_TIMEOUT_MS;

    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        // A helper that stops answering is not coming back: kill it so the next
        // call gets a fresh process instead of queueing behind a dead one.
        this.killHelper('request timed out');
        reject(new Error(`${method}: no response within ${timeoutMs}ms`));
      }, timeoutMs);

      this.pending.set(id, { resolve, reject, timer, method });

      const payload = JSON.stringify({
        jsonrpc: '2.0',
        id,
        method,
        ...(params ? { params } : {}),
      });
      try {
        child.stdin?.write(`${payload}\n`);
      } catch (error) {
        this.pending.delete(id);
        clearTimeout(timer);
        reject(error instanceof Error ? error : new Error(String(error)));
      }
    });
  }

  killHelper(reason) {
    console.error(`[ComputerUseLinux] killing helper: ${reason}`);
    try {
      this.child?.kill('SIGKILL');
    } catch {
      // already gone
    }
    this.child = null;
  }

  // ------------------------------------------------------------ typed surface

  async list_windows() {
    const result = await this.request('list_windows');
    // The Linux helper wraps the array as { windows: [...] }; the bridge
    // expects the bare array it receives from the macOS/Windows helpers.
    if (Array.isArray(result)) return result;
    return Array.isArray(result?.windows) ? result.windows : [];
  }

  async list_apps(options) {
    return this.request('list_apps', options);
  }

  async launch_app(options) {
    return this.request('launch_app', options);
  }

  async resolve_app(options) {
    return this.request('resolve_app', options);
  }

  async get_window(options) {
    return this.request('get_window', options);
  }

  async get_window_state(options) {
    return this.request('get_window_state', options);
  }

  async activate_window(options) {
    return this.request('activate_window', options);
  }

  async close(options) {
    return this.request('close', options);
  }

  /** Closes a window (distinct from close(), which shuts the helper down). */
  async close_window(options) {
    return this.request('close', options);
  }

  async click(options) {
    return this.request('click', options);
  }

  async press_key(options) {
    return this.request('press_key', options);
  }

  async type_text(options) {
    return this.request('type_text', options);
  }

  async scroll(options) {
    return this.request('scroll', options);
  }

  async drag(options) {
    return this.request('drag', options);
  }

  async set_value(options) {
    return this.request('set_value', options);
  }

  async perform_secondary_action(options) {
    return this.request('perform_secondary_action', options);
  }

  async check_permissions() {
    return this.request('check_permissions');
  }

  async request_permissions() {
    return this.request('request_permissions');
  }

  /**
   * Reports which backend each verb will take on this desktop and session type.
   *
   * Linux needs this in a way macOS and Windows do not: the same call behaves
   * differently on KDE/X11, KDE/Wayland, GNOME and XFCE, and on Wayland some of
   * them can only work after the user grants a portal permission. Knowing that
   * up front is the difference between planning a task and discovering the
   * layout by failing at it.
   */
  async describe_environment() {
    return this.request('describe_environment');
  }

  /**
   * Opens the Wayland consent dialog as its own step.
   *
   * Without this the first click or keystroke blocks until the user answers,
   * which the MCP layer sees as a slow call and may time out — after which the
   * real request lands on a helper that has been killed. Asking first puts the
   * wait where the model can explain it to the user.
   */
  async request_input_access() {
    return this.request('request_input_access');
  }

  /** GNOME only: toggles org.gnome.desktop.interface toolkit-accessibility. */
  async set_accessibility(options) {
    return this.request('set_accessibility', options);
  }

  async close_helper() {
    if (!this.child) {
      return;
    }
    try {
      await this.request('shutdown');
    } catch {
      // shutdown is best effort; the kill below is the real guarantee
    }
    this.killHelper('client closed');
  }

  /**
   * Tears down the helper process.
   *
   * Note the name collision with the MCP tool `close` (which closes a *window*):
   * the MCP server calls `client.close()` only from restartClient(), so that is
   * the lifecycle meaning here. Window closing goes through `close_window`.
   */
  async close() {
    this.closed = true;
    await this.close_helper();
  }
}

/** Alias matching the shape the MCP server looks for. */
export const LinuxComputerUseClient = ComputerUseClient;
export const knownMethods = KNOWN_METHODS;

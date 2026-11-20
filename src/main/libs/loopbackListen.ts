import * as net from 'net';
import type { Server } from 'net';

const MIN_PREFERRED_PORT = 1024;
const MAX_PORT = 65_535;
const FALLBACK_ERROR_CODES = new Set(['EADDRINUSE', 'EACCES', 'EADDRNOTAVAIL']);
// When the preferred port is taken, walk downward at most this many ports
// before giving up and letting the OS pick an ephemeral one.
const DEFAULT_DECREMENT_ATTEMPTS = 100;

export function isUsableLoopbackPort(port: unknown): port is number {
  return typeof port === 'number' && Number.isInteger(port) && port >= MIN_PREFERRED_PORT && port <= MAX_PORT;
}

/**
 * Probe whether a loopback port can be bound right now. Best effort: between
 * the probe and the real bind another process may still take the port.
 */
export function isLoopbackPortAvailable(port: number, host = '127.0.0.1'): Promise<boolean> {
  return new Promise((resolve) => {
    if (!isUsableLoopbackPort(port)) {
      resolve(false);
      return;
    }
    const server = net.createServer();
    server.once('error', () => resolve(false));
    server.once('listening', () => {
      server.close(() => resolve(true));
    });
    server.listen(port, host);
  });
}

/**
 * Find a free port by trying `preferredPort` first, then decrementing
 * (preferredPort - 1, preferredPort - 2, ...) up to `maxAttempts` ports.
 * Returns null when every candidate is occupied; the caller decides on a
 * fallback (e.g. an ephemeral port).
 */
export async function findAvailablePortDescending(
  preferredPort: number,
  options: { maxAttempts?: number; host?: string } = {},
): Promise<number | null> {
  const maxAttempts = options.maxAttempts ?? DEFAULT_DECREMENT_ATTEMPTS;
  const host = options.host ?? '127.0.0.1';
  const floor = Math.max(MIN_PREFERRED_PORT, preferredPort - maxAttempts + 1);
  for (let port = preferredPort; port >= floor; port -= 1) {
    if (await isLoopbackPortAvailable(port, host)) {
      return port;
    }
  }
  return null;
}

function listenOnce(server: Server, host: string, port: number): Promise<number> {
  return new Promise((resolve, reject) => {
    const onError = (error: Error) => {
      server.off('listening', onListening);
      reject(error);
    };
    const onListening = () => {
      server.off('error', onError);
      const address = server.address();
      if (address && typeof address === 'object') {
        resolve(address.port);
      } else {
        reject(new Error(`Loopback server on ${host} has no bound port`));
      }
    };
    server.once('error', onError);
    server.once('listening', onListening);
    server.listen(port, host);
  });
}

/**
 * Bind a loopback server, reusing the previous port while it is free. Ports
 * rendered into openclaw.json must stay stable across launches, otherwise
 * OpenClaw's startup-migration checkpoint misses on every gateway start.
 *
 * When the preferred port is occupied, decrement (preferredPort - 1, -2, ...)
 * until a free port binds, so consecutive launches land on predictable ports
 * instead of random ephemeral ones. Only after the whole decrement range is
 * exhausted do we fall back to an OS-assigned ephemeral port.
 */
export async function listenOnLoopback(
  server: Server,
  host: string,
  preferredPort?: number | null,
): Promise<{ port: number; reused: boolean }> {
  if (isUsableLoopbackPort(preferredPort)) {
    const floor = Math.max(MIN_PREFERRED_PORT, preferredPort - DEFAULT_DECREMENT_ATTEMPTS + 1);
    for (let port = preferredPort; port >= floor; port -= 1) {
      try {
        return { port: await listenOnce(server, host, port), reused: port === preferredPort };
      } catch (error) {
        const code = (error as NodeJS.ErrnoException).code;
        if (!code || !FALLBACK_ERROR_CODES.has(code)) throw error;
        if (port === preferredPort) {
          console.warn(`[Loopback] preferred port ${preferredPort} on ${host} is unavailable (${code}); decrementing`);
        }
      }
    }
    console.warn(`[Loopback] no free port below ${preferredPort} after ${DEFAULT_DECREMENT_ATTEMPTS} attempts; using an ephemeral port`);
  }
  return { port: await listenOnce(server, host, 0), reused: false };
}

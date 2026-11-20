import net from 'net';
import { afterEach, expect, test } from 'vitest';

import { findAvailablePortDescending, isLoopbackPortAvailable, isUsableLoopbackPort, listenOnLoopback } from './loopbackListen';

const servers: net.Server[] = [];

afterEach(async () => {
  await Promise.all(servers.splice(0).map(server => new Promise<void>(resolve => {
    if (!server.listening) {
      resolve();
      return;
    }
    server.close(() => resolve());
  })));
});

const track = (server: net.Server) => {
  servers.push(server);
  return server;
};

async function reserveFreePort(): Promise<number> {
  const probe = net.createServer();
  const { port } = await listenOnLoopback(probe, '127.0.0.1');
  await new Promise<void>(resolve => probe.close(() => resolve()));
  return port;
}

test('reuses the preferred port while it is free', async () => {
  const preferred = await reserveFreePort();
  const result = await listenOnLoopback(track(net.createServer()), '127.0.0.1', preferred);
  expect(result).toEqual({ port: preferred, reused: true });
});

test('decrements to the nearest free port when the preferred port is taken', async () => {
  const holder = track(net.createServer());
  const { port: taken } = await listenOnLoopback(holder, '127.0.0.1');
  const server = track(net.createServer());
  const result = await listenOnLoopback(server, '127.0.0.1', taken);
  expect(result.reused).toBe(false);
  expect(result.port).toBe(taken - 1);
  expect(server.listening).toBe(true);
});

test('falls back to an ephemeral port when the whole decrement range is occupied', async () => {
  const first = track(net.createServer());
  const { port: taken } = await listenOnLoopback(first, '127.0.0.1');
  const second = track(net.createServer());
  const { port: takenLower } = await listenOnLoopback(second, '127.0.0.1', taken - 1);
  expect(takenLower).toBe(taken - 1);
  const server = track(net.createServer());
  const result = await listenOnLoopback(server, '127.0.0.1', taken);
  expect(result.reused).toBe(false);
  expect(result.port).not.toBe(taken);
  expect(result.port).not.toBe(taken - 1);
  expect(server.listening).toBe(true);
});

test('findAvailablePortDescending walks downward and probes availability', async () => {
  const holder = track(net.createServer());
  const { port: taken } = await listenOnLoopback(holder, '127.0.0.1');
  expect(await isLoopbackPortAvailable(taken)).toBe(false);
  expect(await isLoopbackPortAvailable(taken - 1)).toBe(true);
  expect(await findAvailablePortDescending(taken, { maxAttempts: 2 })).toBe(taken - 1);
  expect(await findAvailablePortDescending(taken, { maxAttempts: 1 })).toBeNull();
  expect(await findAvailablePortDescending(80)).toBeNull();
});

test('ignores preferred ports outside the unprivileged range', async () => {
  expect(isUsableLoopbackPort(80)).toBe(false);
  expect(isUsableLoopbackPort(70_000)).toBe(false);
  expect(isUsableLoopbackPort(4121.5)).toBe(false);
  expect(isUsableLoopbackPort('4121')).toBe(false);
  expect(isUsableLoopbackPort(4121)).toBe(true);
  const result = await listenOnLoopback(track(net.createServer()), '127.0.0.1', 80);
  expect(result.reused).toBe(false);
});

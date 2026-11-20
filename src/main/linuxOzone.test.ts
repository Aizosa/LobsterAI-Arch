import { describe, expect, test, vi } from 'vitest';

// The module reaches for Electron's app at import time; nothing here restarts a real process.
vi.mock('electron', () => ({ app: { relaunch: vi.fn(), exit: vi.fn() } }));

import { isWaylandSession, linuxOzoneRelaunchArgs } from './linuxOzone';

const wayland: NodeJS.ProcessEnv = { XDG_SESSION_TYPE: 'wayland', WAYLAND_DISPLAY: 'wayland-1', DISPLAY: ':0' };
const x11: NodeJS.ProcessEnv = { XDG_SESSION_TYPE: 'x11', DISPLAY: ':0' };
/** argv as Electron builds it: the executable first, then the app path, then the app's own flags. */
const argv = ['/usr/lib/electron/electron', '.', '--no-sandbox'];

describe('isWaylandSession', () => {
  test('reads the session type', () => {
    expect(isWaylandSession(wayland)).toBe(true);
    expect(isWaylandSession(x11)).toBe(false);
  });

  test('accepts the socket name alone, since targeted launches miss the session variables', () => {
    expect(isWaylandSession({ WAYLAND_DISPLAY: 'wayland-0' })).toBe(true);
    expect(isWaylandSession({ DISPLAY: ':0' })).toBe(false);
    expect(isWaylandSession({})).toBe(false);
  });
});

describe('linuxOzoneRelaunchArgs', () => {
  test('asks for XWayland on a Wayland session', () => {
    const args = linuxOzoneRelaunchArgs(argv, wayland);
    expect(args).toContain('--ozone-platform=x11');
    expect(args?.[0]).toBe('.');
  });

  test('leaves an X11 session alone', () => {
    expect(linuxOzoneRelaunchArgs(argv, x11)).toBeNull();
  });

  // The cases below all decide "someone already chose"; getting one wrong means an app that
  // restarts on every launch.
  test('does not overrule an explicit platform, whichever way it was given', () => {
    expect(linuxOzoneRelaunchArgs([...argv, '--ozone-platform=x11'], wayland)).toBeNull();
    expect(linuxOzoneRelaunchArgs([...argv, '--ozone-platform=wayland'], wayland)).toBeNull();
    expect(linuxOzoneRelaunchArgs([...argv, '--ozone-platform', 'x11'], wayland)).toBeNull();
  });

  test('does not overrule the hint variable older Electron versions read', () => {
    expect(linuxOzoneRelaunchArgs(argv, { ...wayland, ELECTRON_OZONE_PLATFORM_HINT: 'x11' })).toBeNull();
  });

  test('honours the opt-out', () => {
    expect(linuxOzoneRelaunchArgs(argv, { ...wayland, LOBSTERAI_OZONE_PLATFORM: 'native' })).toBeNull();
  });

  test('never relaunches twice, so a platform that still picks Wayland cannot loop', () => {
    expect(linuxOzoneRelaunchArgs([...argv, '--lobsterai-ozone-relaunched'], wayland)).toBeNull();
  });

  test('marks the restarted process so the guard survives a platform that ignores the flag', () => {
    const args = linuxOzoneRelaunchArgs(argv, wayland);
    expect(args).toContain('--lobsterai-ozone-relaunched');
    // Feeding the result back in must come out empty.
    expect(linuxOzoneRelaunchArgs(['/usr/bin/electron', ...args ?? []], wayland)).toBeNull();
  });

  test('keeps the flags the user launched with', () => {
    const args = linuxOzoneRelaunchArgs([...argv, '--enable-logging'], wayland);
    expect(args).toContain('--enable-logging');
    expect(args?.[0]).toBe('.');
  });
});

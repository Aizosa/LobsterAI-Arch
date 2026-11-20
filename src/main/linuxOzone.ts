/**
 * Electron has to be told to run on XWayland before Chromium picks an Ozone platform, which happens
 * long before any JavaScript runs. `app.commandLine.appendSwitch('ozone-platform', 'x11')` is
 * therefore too late — measured on Electron 44 / KWin 6.8: the window does become an X client and
 * `setBounds` starts working, but `screen` keeps reporting the Wayland geometry it already cached.
 * The result is worse than not switching at all, because every companion surface would be placed
 * confidently in the wrong coordinate space.
 *
 * So the only reliable lever is the process command line, and the only way to change that from
 * inside the app is to restart it once with the flag added.
 *
 * Why this matters for LobsterAI specifically: the desktop companion positions every surface with
 * `BrowserWindow.setBounds()` against `display.workArea`. On a native Wayland session both are
 * broken — the protocol forbids a client from setting its own global position, and `workArea` comes
 * back equal to `bounds` because the panel height is not exposed. The selection toolbar would open
 * at (0, 0), and the answer card, stage and permission guide would miss their anchors.
 */
import { app } from 'electron';

const OZONE_FLAG = '--ozone-platform';
/**
 * Passed to the restarted process. `app.relaunch` only accepts `args` and `execPath` — it has no
 * `env`, so a sentinel cannot travel through the environment — and this is what stops a platform
 * that still picks Wayland from restarting forever.
 */
const RELAUNCH_FLAG = '--lobsterai-ozone-relaunched';
/** Set to `native` to keep a native Wayland session and accept that companion surfaces cannot be placed. */
const OPT_OUT_ENV = 'LOBSTERAI_OZONE_PLATFORM';

/** Whether this session draws through a Wayland compositor, whatever the app itself will use. */
export function isWaylandSession(env: NodeJS.ProcessEnv): boolean {
  // XDG_SESSION_TYPE is what the session manager exports; WAYLAND_DISPLAY survives targeted
  // launches (a .desktop file, an IDE task) that do not inherit the full session environment.
  if (env.XDG_SESSION_TYPE === 'wayland') return true;
  // The value is a socket name such as `wayland-0` or `wayland-1`, or a path; any of them means
  // a compositor is there to connect to.
  return !!env.WAYLAND_DISPLAY;
}

/**
 * The arguments to restart with, or null when the app should keep the platform it was given.
 *
 * Split out from the relaunch itself so this decision stays testable: which argv counts as "already
 * decided" is the part that goes wrong, and a wrong answer here means an app that restarts on every
 * launch.
 */
export function linuxOzoneRelaunchArgs(argv: readonly string[], env: NodeJS.ProcessEnv): string[] | null {
  if (env[OPT_OUT_ENV] === 'native') return null;
  // A relaunch already happened; whatever platform we ended up on is the answer.
  if (argv.includes(RELAUNCH_FLAG)) return null;
  if (!isWaylandSession(env)) return null;
  // Any explicit choice wins, including `--ozone-platform=wayland` from a user who wants native.
  if (argv.some(arg => arg === OZONE_FLAG || arg.startsWith(`${OZONE_FLAG}=`))) return null;
  // Electron below 38 reads the hint variable instead; honour it rather than overrule it.
  if (env.ELECTRON_OZONE_PLATFORM_HINT) return null;
  // `process.argv` starts with the executable; `app.relaunch` wants what follows it.
  return [...argv.slice(1), `${OZONE_FLAG}=x11`, RELAUNCH_FLAG];
}

/**
 * Restarts the app on XWayland when a Wayland session would break window placement.
 *
 * Must run before `app.whenReady()` and before any lock or file is taken, so the discarded first
 * process leaves nothing behind.
 */
export function applyLinuxOzone(): void {
  if (process.platform !== 'linux') return;
  const args = linuxOzoneRelaunchArgs(process.argv, process.env);
  if (!args) return;
  console.log('[Ozone] Wayland session detected; restarting on XWayland so companion windows can be positioned');
  app.relaunch({ args });
  app.exit(0);
}

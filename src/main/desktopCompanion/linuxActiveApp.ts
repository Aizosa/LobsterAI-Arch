/**
 * Which application is in front, on Linux.
 *
 * macOS has NSWorkspace and Windows has a foreground-window call; a Wayland session has neither,
 * because the protocol deliberately hides other clients from each other. What is left is per-desktop:
 * GNOME exports an introspectable window list, Sway and Hyprland print their tree on request, and
 * every X11 session — plus XWayland, which is where Electron and most Chromium apps live — answers
 * through `_NET_ACTIVE_WINDOW`.
 *
 * The answers are read through the distribution's own tools (`busctl`, `xprop`, `swaymsg`,
 * `hyprctl`) rather than a D-Bus or Xlib binding, so nothing new has to be shipped or built per
 * architecture, and a desktop that does not have the tool simply reports nothing.
 *
 * A null return is a normal answer, not an error. Callers treat it as "cannot tell", and the
 * selection toolbar then shows without the ability to skip that app.
 */
import { execFile } from 'child_process';

export const LinuxDesktop = {
  Kde: 'kde',
  Gnome: 'gnome',
  Xfce: 'xfce',
  Sway: 'sway',
  Hyprland: 'hyprland',
  Other: 'other',
} as const;
export type LinuxDesktop = typeof LinuxDesktop[keyof typeof LinuxDesktop];

/** Long enough for a cold D-Bus round trip, short enough not to pile up behind a 1.5 s poll. */
const TOOL_TIMEOUT_MS = 700;

/** `XDG_CURRENT_DESKTOP` is a colon-separated list such as `KDE` or `ubuntu:GNOME`. */
export function detectLinuxDesktop(env: NodeJS.ProcessEnv): LinuxDesktop {
  const names = (env.XDG_CURRENT_DESKTOP ?? '')
    .split(':')
    .map(name => name.trim().toLowerCase())
    .filter(Boolean);
  if (names.some(name => name.includes('kde') || name === 'plasma')) return LinuxDesktop.Kde;
  if (names.some(name => name.includes('gnome') || name === 'unity' || name === 'ubuntu')) return LinuxDesktop.Gnome;
  if (names.some(name => name.includes('xfce'))) return LinuxDesktop.Xfce;
  if (names.some(name => name.includes('sway'))) return LinuxDesktop.Sway;
  if (names.some(name => name.includes('hyprland'))) return LinuxDesktop.Hyprland;
  // Compositors that set only their own variable, without a desktop name.
  if (env.SWAYSOCK) return LinuxDesktop.Sway;
  if (env.HYPRLAND_INSTANCE_SIGNATURE) return LinuxDesktop.Hyprland;
  return LinuxDesktop.Other;
}

/**
 * GNOME's window list, as `busctl --json=short` prints it: `{"type":"a{ta{sv}}","data":[[[id,{...}]]]}`.
 *
 * The focused window carries `wm-class` and `focus: true`. Windows without a class (menus, tooltips)
 * are skipped rather than guessed at.
 */
export function parseGnomeIntrospect(json: string): string | null {
  const parsed = JSON.parse(json) as { data?: unknown };
  // `data` is the single out-argument of GetWindows: a list of [id, properties] pairs.
  const entries = Array.isArray(parsed.data) ? parsed.data : [];
  const windows = Array.isArray(entries[0]) ? entries[0] as unknown[] : [];
  let focusedClass: string | null = null;
  for (const entry of windows) {
    if (!Array.isArray(entry) || entry.length < 2) continue;
    const props = entry[1];
    if (!props || typeof props !== 'object') continue;
    if (readVariantBool((props as Record<string, unknown>).focus) !== true) continue;
    const wmClass = readVariantString((props as Record<string, unknown>)['wm-class']);
    // A focused window can lack a class; keep looking rather than settle for null.
    if (wmClass) focusedClass = wmClass;
  }
  return focusedClass;
}

/** Sway's tree, as `swaymsg -t get_tree` prints it: windows nest inside containers. */
export function parseSwayTree(json: string): string | null {
  const root = JSON.parse(json) as SwayNode;
  const queue: SwayNode[] = [root];
  while (queue.length) {
    const node = queue.shift();
    if (!node) continue;
    if (node.focused) {
      const appId = typeof node.app_id === 'string' && node.app_id ? node.app_id : null;
      const instance = typeof node.window_properties?.class === 'string' ? node.window_properties.class : null;
      if (appId || instance) return appId ?? instance;
    }
    if (Array.isArray(node.nodes)) queue.push(...node.nodes);
    if (Array.isArray(node.floating_nodes)) queue.push(...node.floating_nodes);
  }
  return null;
}

interface SwayNode {
  focused?: boolean;
  app_id?: string | null;
  window_properties?: { class?: string | null } | null;
  nodes?: SwayNode[];
  floating_nodes?: SwayNode[];
}

/** Hyprland's `hyprctl activewindow -j`. */
export function parseHyprlandActiveWindow(json: string): string | null {
  const parsed = JSON.parse(json) as { class?: unknown; initialClass?: unknown };
  const value = typeof parsed.class === 'string' && parsed.class
    ? parsed.class
    : (typeof parsed.initialClass === 'string' ? parsed.initialClass : null);
  return value || null;
}

/** `_NET_ACTIVE_WINDOW` as `xprop -root` prints it. Zero and "no such atom" both mean nothing. */
export function parseActiveWindowId(stdout: string): string | null {
  const match = /window id # (0x[0-9a-fA-F]+)/.exec(stdout);
  if (!match) return null;
  const id = match[1];
  // 0x0 is what a window manager sets when nothing is focused.
  return /^0x0+$/.test(id) ? null : id;
}

/**
 * `WM_CLASS(STRING) = "instance", "class"` as `xprop -id <id> WM_CLASS` prints it.
 *
 * The instance name is returned because it is the executable name lowercased, which is what the
 * exclusion lists are written in on Windows. The class is the fallback for the few apps that leave
 * the instance empty.
 */
export function parseWmClass(stdout: string): string | null {
  const match = /WM_CLASS\(STRING\)\s*=\s*"([^"]*)"(?:,\s*"([^"]*)")?/.exec(stdout);
  if (!match) return null;
  const instance = match[1]?.trim();
  const klass = match[2]?.trim();
  const value = instance || klass;
  return value ? value.toLowerCase() : null;
}

function readVariantString(value: unknown): string | null {
  const data = (value as { data?: unknown } | null)?.data;
  return Array.isArray(data) && typeof data[0] === 'string' && data[0] ? data[0] : null;
}

function readVariantBool(value: unknown): boolean | null {
  const data = (value as { data?: unknown } | null)?.data;
  return Array.isArray(data) && typeof data[0] === 'boolean' ? data[0] : null;
}

function run(command: string, args: string[]): Promise<string | null> {
  return new Promise(resolve => {
    execFile(command, args, { timeout: TOOL_TIMEOUT_MS, maxBuffer: 4 * 1024 * 1024 }, (error, stdout) => {
      resolve(error ? null : stdout);
    });
  });
}

/** The window manager's active window, through X11. Works on any X11 session and over XWayland. */
async function readActiveAppFromX11(): Promise<string | null> {
  const active = await run('xprop', ['-root', '_NET_ACTIVE_WINDOW']);
  const id = active ? parseActiveWindowId(active) : null;
  if (!id) return null;
  const props = await run('xprop', ['-id', id, 'WM_CLASS']);
  return props ? parseWmClass(props) : null;
}

async function readActiveAppFromGnome(): Promise<string | null> {
  const output = await run('busctl', [
    '--json=short', 'call', '--user',
    'org.gnome.Shell.Introspect', '/org/gnome/Shell/Introspect',
    'org.gnome.Shell.Introspect', 'GetWindows',
  ]);
  if (!output) return null;
  try {
    return parseGnomeIntrospect(output);
  } catch {
    return null;
  }
}

async function readActiveAppFromSway(): Promise<string | null> {
  const output = await run('swaymsg', ['-t', 'get_tree', '-r']);
  if (!output) return null;
  try {
    return parseSwayTree(output);
  } catch {
    return null;
  }
}

async function readActiveAppFromHyprland(): Promise<string | null> {
  const output = await run('hyprctl', ['activewindow', '-j']);
  if (!output) return null;
  try {
    return parseHyprlandActiveWindow(output);
  } catch {
    return null;
  }
}

/**
 * The frontmost application's name, or null when this session will not say.
 *
 * The desktop's own interface is tried first where one exists, then X11, which covers XWayland on
 * every desktop. On KWin the X11 result is the only one available for a Wayland session: KWin
 * exposes no non-interactive way to read its active window, so a native Wayland window there
 * reports nothing.
 */
export async function readActiveAppId(env: NodeJS.ProcessEnv = process.env): Promise<string | null> {
  const desktop = detectLinuxDesktop(env);
  switch (desktop) {
    case LinuxDesktop.Gnome:
      return (await readActiveAppFromGnome()) ?? readActiveAppFromX11();
    case LinuxDesktop.Sway:
      return (await readActiveAppFromSway()) ?? readActiveAppFromX11();
    case LinuxDesktop.Hyprland:
      return (await readActiveAppFromHyprland()) ?? readActiveAppFromX11();
    default:
      // KDE, XFCE and anything else with an X display.
      return readActiveAppFromX11();
  }
}

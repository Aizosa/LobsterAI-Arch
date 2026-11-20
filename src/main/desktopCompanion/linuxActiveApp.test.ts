import { describe, expect, test } from 'vitest';

import {
  detectLinuxDesktop,
  LinuxDesktop,
  parseActiveWindowId,
  parseGnomeIntrospect,
  parseHyprlandActiveWindow,
  parseSwayTree,
  parseWmClass,
} from './linuxActiveApp';

describe('detectLinuxDesktop', () => {
  test('reads the colon-separated desktop list', () => {
    expect(detectLinuxDesktop({ XDG_CURRENT_DESKTOP: 'KDE' })).toBe(LinuxDesktop.Kde);
    expect(detectLinuxDesktop({ XDG_CURRENT_DESKTOP: 'ubuntu:GNOME' })).toBe(LinuxDesktop.Gnome);
    expect(detectLinuxDesktop({ XDG_CURRENT_DESKTOP: 'XFCE' })).toBe(LinuxDesktop.Xfce);
    expect(detectLinuxDesktop({ XDG_CURRENT_DESKTOP: 'sway' })).toBe(LinuxDesktop.Sway);
    expect(detectLinuxDesktop({ XDG_CURRENT_DESKTOP: 'Hyprland' })).toBe(LinuxDesktop.Hyprland);
  });

  test('recognizes the compositor variable when no desktop name is set', () => {
    expect(detectLinuxDesktop({ SWAYSOCK: '/run/user/1000/sway.sock' })).toBe(LinuxDesktop.Sway);
    expect(detectLinuxDesktop({ HYPRLAND_INSTANCE_SIGNATURE: 'abc' })).toBe(LinuxDesktop.Hyprland);
  });

  test('handles a session that names nothing', () => {
    expect(detectLinuxDesktop({})).toBe(LinuxDesktop.Other);
    expect(detectLinuxDesktop({ XDG_CURRENT_DESKTOP: '' })).toBe(LinuxDesktop.Other);
  });

  test('does not mistake a bare XFCE session for GNOME', () => {
    expect(detectLinuxDesktop({ XDG_CURRENT_DESKTOP: 'XFCE' })).not.toBe(LinuxDesktop.Gnome);
  });
});

// The exact shape busctl --json=short prints for a{ta{sv}}, checked against the interface definition
// rather than a live GNOME session: entry is [window id, {property: {type, data}}].
const gnomeIntrospect = JSON.stringify({
  type: 'a{ta{sv}}',
  data: [[
    [4194307, {
      'wm-class': { type: 's', data: ['org.gnome.Nautilus'] },
      focus: { type: 'b', data: [false] },
      title: { type: 's', data: ['Home'] },
    }],
    [4194308, {
      'wm-class': { type: 's', data: ['firefox'] },
      focus: { type: 'b', data: [true] },
      title: { type: 's', data: ['Mozilla Firefox'] },
    }],
  ]],
});

describe('parseGnomeIntrospect', () => {
  test('picks the focused window, not the first one', () => {
    expect(parseGnomeIntrospect(gnomeIntrospect)).toBe('firefox');
  });

  test('returns nothing when no window is focused', () => {
    const idle = JSON.stringify({ type: 'a{ta{sv}}', data: [[[1, { 'wm-class': { type: 's', data: ['firefox'] }, focus: { type: 'b', data: [false] } }]]] });
    expect(parseGnomeIntrospect(idle)).toBeNull();
  });

  test('skips a focused window that carries no class, such as a menu', () => {
    const menu = JSON.stringify({
      type: 'a{ta{sv}}',
      data: [[
        [1, { 'wm-class': { type: 's', data: ['firefox'] }, focus: { type: 'b', data: [false] } }],
        [2, { focus: { type: 'b', data: [true] } }],
      ]],
    });
    expect(parseGnomeIntrospect(menu)).toBeNull();
  });

  test('survives an empty window list', () => {
    expect(parseGnomeIntrospect(JSON.stringify({ type: 'a{ta{sv}}', data: [[]] }))).toBeNull();
  });
});

describe('parseSwayTree', () => {
  test('finds the focused leaf through nested containers', () => {
    const tree = JSON.stringify({
      type: 'root',
      nodes: [{
        type: 'output',
        nodes: [{
          type: 'con',
          nodes: [
            { type: 'con', name: 'terminal', app_id: 'Alacritty', focused: false },
            // A native Wayland client names itself with app_id; an XWayland one only has a class.
            { type: 'con', name: 'browser', app_id: 'firefox', focused: true },
          ],
        }],
      }],
    });
    expect(parseSwayTree(tree)).toBe('firefox');
  });

  test('falls back to the X11 class for an XWayland window', () => {
    const tree = JSON.stringify({
      nodes: [{ nodes: [{ focused: true, app_id: null, window_properties: { class: 'Thunar' } }] }],
    });
    expect(parseSwayTree(tree)).toBe('Thunar');
  });

  test('returns nothing when no window has focus', () => {
    expect(parseSwayTree(JSON.stringify({ nodes: [{ app_id: 'a', focused: false }] }))).toBeNull();
  });
});

describe('parseHyprlandActiveWindow', () => {
  test('reads the class of the active window', () => {
    expect(parseHyprlandActiveWindow(JSON.stringify({ class: 'kitty', title: 'shell' }))).toBe('kitty');
  });

  test('handles an empty desktop, which prints a title-only object', () => {
    expect(parseHyprlandActiveWindow(JSON.stringify({ title: 'workspace' }))).toBeNull();
    expect(parseHyprlandActiveWindow(JSON.stringify({}))).toBeNull();
  });
});

// The two strings below are copied verbatim from xprop on a KDE Plasma 6 XWayland session.
describe('parseActiveWindowId', () => {
  test('reads the window id', () => {
    expect(parseActiveWindowId('_NET_ACTIVE_WINDOW(WINDOW): window id # 0x1200004\n')).toBe('0x1200004');
  });

  test('treats no focus and a missing atom as nothing', () => {
    expect(parseActiveWindowId('_NET_ACTIVE_WINDOW(WINDOW): window id # 0x0\n')).toBeNull();
    expect(parseActiveWindowId('_NET_ACTIVE_WINDOW:  no such atom on any window.\n')).toBeNull();
    expect(parseActiveWindowId('')).toBeNull();
  });
});

describe('parseWmClass', () => {
  test('returns the instance name lowercased, which is the executable name', () => {
    expect(parseWmClass('WM_CLASS(STRING) = "workbuddy", "WorkBuddy"\n')).toBe('workbuddy');
  });

  test('falls back to the class when the instance is empty', () => {
    expect(parseWmClass('WM_CLASS(STRING) = "", "Thunar"\n')).toBe('thunar');
  });

  test('returns nothing when the property is absent', () => {
    expect(parseWmClass('WM_CLASS:  not found.\n')).toBeNull();
  });
});

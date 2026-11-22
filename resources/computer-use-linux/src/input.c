/*
 * Input injection.
 *
 * Three layers, deliberately separated:
 *
 *   x11_*      XTest. Needs no permission at all, and reaches every X client.
 *   wl_*       RemoteDesktop portal. On Wayland this is the only sanctioned
 *              route; the user consents once per process and we reuse the grant.
 *   cu_input_* the public surface. Picks a backend per session type and falls
 *              through from one to the other, so callers never branch.
 *
 * The two backends are not equivalent even when both exist (an XWayland session
 * has both): XTest reaches only X11 clients, while the portal reaches every
 * client on the compositor. So on Wayland we prefer the portal and use XTest
 * only as a fallback when the portal is missing.
 *
 * Key chords: "ctrl+shift+t", "alt+left", "return". Modifier names follow the
 * MCP layer's vocabulary (cmd/ctrl/alt/option/shift/fn); "cmd" maps to Super on
 * Linux, matching how Linux keyboards label the key.
 */
#include "cu.h"

#include <string.h>

#if defined(CU_HAVE_X11)
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#endif

/* Written last so every definition above is visible. */
static bool prefer_wayland_input(void) {
  const char *session = cu_session_type();
  if (strcmp(session, "wayland") == 0) return true;
  /* An explicit "x11" wins; otherwise a Wayland socket plus an X display means
     XWayland, where native Wayland clients are invisible to XTest. */
  if (strcmp(session, "x11") == 0) return false;
  return g_getenv("WAYLAND_DISPLAY") != NULL;
}

void cu_input_init(void) {}

/*
 * Name -> X keysym without an X server.
 *
 * The Wayland path needs keysyms, and the X11 path resolves them via
 * XStringToKeysym — but that needs a display, which a pure Wayland session may
 * not have (or may have only under XWayland, which can be gone). So the common
 * names are hard-coded here and both paths share them; XStringToKeysym remains
 * as the X11-only catch-all for exotic names.
 *
 * The numeric values are from <X11/keysymdef.h> and are ABI-stable: they are
 * part of the X11 protocol, not of any library version.
 */
unsigned long cu_keysym_from_name(const char *name) {
  if (!name || !*name) return 0;

  if (g_ascii_strcasecmp(name, "return") == 0 ||
      g_ascii_strcasecmp(name, "enter") == 0)   return 0xff0d;
  if (g_ascii_strcasecmp(name, "tab") == 0)     return 0xff09;
  if (g_ascii_strcasecmp(name, "escape") == 0 ||
      g_ascii_strcasecmp(name, "esc") == 0)     return 0xff1b;
  if (g_ascii_strcasecmp(name, "space") == 0)   return 0x0020;
  if (g_ascii_strcasecmp(name, "backspace") == 0) return 0xff08;
  if (g_ascii_strcasecmp(name, "delete") == 0 ||
      g_ascii_strcasecmp(name, "del") == 0)     return 0xffff;
  if (g_ascii_strcasecmp(name, "insert") == 0)  return 0xff63;
  if (g_ascii_strcasecmp(name, "up") == 0)      return 0xff52;
  if (g_ascii_strcasecmp(name, "down") == 0)    return 0xff54;
  if (g_ascii_strcasecmp(name, "left") == 0)    return 0xff51;
  if (g_ascii_strcasecmp(name, "right") == 0)   return 0xff53;
  if (g_ascii_strcasecmp(name, "pageup") == 0 ||
      g_ascii_strcasecmp(name, "prior") == 0)   return 0xff55;
  if (g_ascii_strcasecmp(name, "pagedown") == 0 ||
      g_ascii_strcasecmp(name, "next") == 0)    return 0xff56;
  if (g_ascii_strcasecmp(name, "home") == 0)    return 0xff50;
  if (g_ascii_strcasecmp(name, "end") == 0)     return 0xff57;
  /* Media keys, which the MCP layer occasionally asks for. */
  if (g_ascii_strcasecmp(name, "audio_mute") == 0)  return 0x1008ff12;
  if (g_ascii_strcasecmp(name, "audio_raise") == 0) return 0x1008ff13;
  if (g_ascii_strcasecmp(name, "audio_lower") == 0) return 0x1008ff11;

  /* Modifier keys. These are valid as standalone press/release targets in the
     model's keypress list, not only as chord prefixes; the chord path resolves
     modifiers locally, but an isolated modifier as the final item still needs
     this table. A bare "shift" resolves to the left variant — symmetric enough
     for a single tap, and a chord with an explicit side would name shift_l/r. */
  if (g_ascii_strcasecmp(name, "shift") == 0 ||
      g_ascii_strcasecmp(name, "shift_l") == 0)  return 0xffe1;
  if (g_ascii_strcasecmp(name, "shift_r") == 0)  return 0xffe2;
  if (g_ascii_strcasecmp(name, "ctrl") == 0 ||
      g_ascii_strcasecmp(name, "control") == 0 ||
      g_ascii_strcasecmp(name, "control_l") == 0) return 0xffe3;
  if (g_ascii_strcasecmp(name, "ctrl_r") == 0 ||
      g_ascii_strcasecmp(name, "control_r") == 0) return 0xffe4;
  if (g_ascii_strcasecmp(name, "alt") == 0 ||
      g_ascii_strcasecmp(name, "alt_l") == 0 ||
      g_ascii_strcasecmp(name, "option") == 0)  return 0xffe9;
  if (g_ascii_strcasecmp(name, "alt_r") == 0 ||
      g_ascii_strcasecmp(name, "altgr") == 0)   return 0xfe03;
  if (g_ascii_strcasecmp(name, "super") == 0 ||
      g_ascii_strcasecmp(name, "super_l") == 0 ||
      g_ascii_strcasecmp(name, "meta") == 0 ||
      g_ascii_strcasecmp(name, "meta_l") == 0 ||
      g_ascii_strcasecmp(name, "cmd") == 0 ||
      g_ascii_strcasecmp(name, "win") == 0)     return 0xffeb;
  if (g_ascii_strcasecmp(name, "super_r") == 0) return 0xffec;
  if (g_ascii_strcasecmp(name, "meta_r") == 0)  return 0xffe8;

  /* Lock / special keys the model sometimes names verbally. */
  if (g_ascii_strcasecmp(name, "capslock") == 0 ||
      g_ascii_strcasecmp(name, "caps_lock") == 0) return 0xffe5;
  if (g_ascii_strcasecmp(name, "menu") == 0)     return 0xff67;
  if (g_ascii_strcasecmp(name, "printscreen") == 0 ||
      g_ascii_strcasecmp(name, "print") == 0 ||
      g_ascii_strcasecmp(name, "prtsc") == 0)    return 0xff61;
  if (g_ascii_strcasecmp(name, "scroll_lock") == 0 ||
      g_ascii_strcasecmp(name, "scrolllock") == 0) return 0xff14;
  if (g_ascii_strcasecmp(name, "pause") == 0 ||
      g_ascii_strcasecmp(name, "break") == 0)    return 0xff13;
  if (g_ascii_strcasecmp(name, "numlock") == 0 ||
      g_ascii_strcasecmp(name, "num_lock") == 0) return 0xff7f;

  /* Function keys: XK_F1 == 0xffbe, ascending. */
  if ((name[0] == 'f' || name[0] == 'F') && g_ascii_isdigit(name[1])) {
    int n = atoi(name + 1);
    if (n >= 1 && n <= 35) return 0xffbeUL + (unsigned long)(n - 1);
  }

  /* A single character maps to its own code point for printable Latin-1. */
  if (strlen(name) == 1) {
    unsigned char c = (unsigned char)name[0];
    if (c >= 0x20 && c < 0x7f) return (unsigned long)c;
  }

  return 0;
}

/* ================================ X11 ==================================== */

#if defined(CU_HAVE_X11)

static Display *open_dpy(void) {
  static Display *cached = NULL;
  if (cached) return cached;
  cached = XOpenDisplay(NULL);
  return cached;
}

bool cu_x11_available(void) {
  return open_dpy() != NULL;
}

static bool x11_move_and_click(double x, double y, int button, int click_count) {
  Display *dpy = open_dpy();
  if (!dpy) return false;

  Window root = DefaultRootWindow(dpy);
  Window ret_root, ret_child;
  int rx, ry, wx, wy;
  unsigned int mask;
  /* Remember where the pointer was so we can put it back. */
  bool have_prev = XQueryPointer(dpy, root, &ret_root, &ret_child, &rx, &ry,
                                 &wx, &wy, &mask);

  XTestFakeMotionEvent(dpy, -1, (int)x, (int)y, CurrentTime);
  XFlush(dpy);

  unsigned int b = button == 3 ? Button3 : button == 2 ? Button2 : Button1;
  for (int i = 0; i < MAX(1, click_count); i++) {
    XTestFakeButtonEvent(dpy, b, True, CurrentTime);
    XTestFakeButtonEvent(dpy, b, False, CurrentTime);
  }
  XFlush(dpy);

  if (have_prev) {
    XTestFakeMotionEvent(dpy, -1, rx, ry, CurrentTime);
    XFlush(dpy);
  }
  return true;
}

static bool x11_drag(double from_x, double from_y, double to_x, double to_y) {
  Display *dpy = open_dpy();
  if (!dpy) return false;

  XTestFakeMotionEvent(dpy, -1, (int)from_x, (int)from_y, CurrentTime);
  XFlush(dpy);
  XTestFakeButtonEvent(dpy, Button1, True, CurrentTime);
  XFlush(dpy);

  /* Intermediate steps keep drag-and-drop implementations happy. */
  const int steps = 12;
  for (int i = 1; i <= steps; i++) {
    double t = (double)i / steps;
    XTestFakeMotionEvent(dpy, -1, (int)(from_x + (to_x - from_x) * t),
                         (int)(from_y + (to_y - from_y) * t), CurrentTime);
    XFlush(dpy);
    g_usleep(12000);
  }
  XTestFakeButtonEvent(dpy, Button1, False, CurrentTime);
  XFlush(dpy);
  return true;
}

static bool x11_scroll(double x, double y, int scroll_x, int scroll_y) {
  Display *dpy = open_dpy();
  if (!dpy) return false;

  XTestFakeMotionEvent(dpy, -1, (int)x, (int)y, CurrentTime);
  XFlush(dpy);

  /* Wheel clicks are ~50px in most toolkits; convert pixel deltas.
     Buttons 4/5 are vertical, 6/7 horizontal — X11 has no named constants for
     the horizontal pair, so they are written numerically. */
  int v = scroll_y / 50;
  int h = scroll_x / 50;
  unsigned int vb = v > 0 ? 5 : 4;
  unsigned int hb = h > 0 ? 7 : 6;
  for (int i = 0; i < ABS(v); i++) {
    XTestFakeButtonEvent(dpy, vb, True, CurrentTime);
    XTestFakeButtonEvent(dpy, vb, False, CurrentTime);
  }
  for (int i = 0; i < ABS(h); i++) {
    XTestFakeButtonEvent(dpy, hb, True, CurrentTime);
    XTestFakeButtonEvent(dpy, hb, False, CurrentTime);
  }
  XFlush(dpy);
  return true;
}

static KeySym keyname_to_keysym(const char *name) {
  if (g_ascii_strcasecmp(name, "return") == 0 || g_ascii_strcasecmp(name, "enter") == 0)
    return XK_Return;
  if (g_ascii_strcasecmp(name, "tab") == 0) return XK_Tab;
  if (g_ascii_strcasecmp(name, "escape") == 0 || g_ascii_strcasecmp(name, "esc") == 0)
    return XK_Escape;
  if (g_ascii_strcasecmp(name, "space") == 0) return XK_space;
  if (g_ascii_strcasecmp(name, "backspace") == 0) return XK_BackSpace;
  if (g_ascii_strcasecmp(name, "delete") == 0) return XK_Delete;
  if (g_ascii_strcasecmp(name, "up") == 0) return XK_Up;
  if (g_ascii_strcasecmp(name, "down") == 0) return XK_Down;
  if (g_ascii_strcasecmp(name, "left") == 0) return XK_Left;
  if (g_ascii_strcasecmp(name, "right") == 0) return XK_Right;
  if (g_ascii_strcasecmp(name, "pageup") == 0 || g_ascii_strcasecmp(name, "prior") == 0)
    return XK_Prior;
  if (g_ascii_strcasecmp(name, "pagedown") == 0 || g_ascii_strcasecmp(name, "next") == 0)
    return XK_Next;
  if (g_ascii_strcasecmp(name, "home") == 0) return XK_Home;
  if (g_ascii_strcasecmp(name, "end") == 0) return XK_End;
  /* Function keys */
  if (name[0] == 'f' && g_ascii_isdigit(name[1])) {
    int n = atoi(name + 1);
    if (n >= 1 && n <= 35) return XK_F1 + (n - 1);
  }
  if (strlen(name) == 1) {
    return (KeySym)name[0];
  }
  /* Fall back to the X keysym name table. */
  return XStringToKeysym(name);
}

static bool x11_press_key(const char *key, int repeat) {
  Display *dpy = open_dpy();
  if (!dpy || !key) return false;

  gchar **parts = g_strsplit(key, "+", CU_MAX_KEYSTROKE);
  int n = g_strv_length(parts);
  if (n == 0) {
    g_strfreev(parts);
    return false;
  }

  KeyCode mods[CU_MAX_KEYSTROKE];
  int nmods = 0;
  for (int i = 0; i < n - 1; i++) {
    const char *name = g_strstrip(parts[i]);
    KeySym sym;
    if (g_ascii_strcasecmp(name, "cmd") == 0 ||
        g_ascii_strcasecmp(name, "meta") == 0 ||
        g_ascii_strcasecmp(name, "super") == 0) {
      sym = XK_Super_L;
    } else if (g_ascii_strcasecmp(name, "ctrl") == 0 ||
               g_ascii_strcasecmp(name, "control") == 0) {
      sym = XK_Control_L;
    } else if (g_ascii_strcasecmp(name, "alt") == 0 ||
               g_ascii_strcasecmp(name, "option") == 0) {
      sym = XK_Alt_L;
    } else if (g_ascii_strcasecmp(name, "shift") == 0) {
      sym = XK_Shift_L;
    } else if (g_ascii_strcasecmp(name, "fn") == 0) {
      /* No Linux equivalent; ignore rather than fail. */
      continue;
    } else {
      sym = keyname_to_keysym(name);
    }
    KeyCode code = XKeysymToKeycode(dpy, sym);
    if (code) mods[nmods++] = code;
  }

  KeySym final_sym = keyname_to_keysym(g_strstrip(parts[n - 1]));
  KeyCode final_code = XKeysymToKeycode(dpy, final_sym);
  if (!final_code) {
    g_strfreev(parts);
    return false;
  }

  for (int r = 0; r < MAX(1, repeat); r++) {
    for (int m = 0; m < nmods; m++) {
      XTestFakeKeyEvent(dpy, mods[m], True, CurrentTime);
    }
    XFlush(dpy);
    /* Let the compositor register the modifiers before the final key arrives;
       a chord sent in one burst can be read as the bare key by some toolkits. */
    g_usleep(8000);
    XTestFakeKeyEvent(dpy, final_code, True, CurrentTime);
    XTestFakeKeyEvent(dpy, final_code, False, CurrentTime);
    for (int m = nmods - 1; m >= 0; m--) XTestFakeKeyEvent(dpy, mods[m], False, CurrentTime);
    XFlush(dpy);
    if (repeat > 1) g_usleep(20000);
  }

  g_strfreev(parts);
  return true;
}

static bool x11_type_text(const char *text) {
  Display *dpy = open_dpy();
  if (!dpy || !text) return false;

  /* Map each character to the keysym that produces it. XTest injects keycodes,
     so characters outside a keysym's repertoire (CJK, emoji) cannot be typed
     this way; those are reported as unsupported rather than silently dropped. */
  bool any_failure = false;
  for (const char *p = text; *p; ) {
    gunichar ch = g_utf8_get_char(p);
    if (ch == '\n' || ch == '\r') {
      x11_press_key("return", 1);
      p = g_utf8_next_char(p);
      continue;
    }
    KeySym sym = 0;
    if (ch < 0x100) {
      sym = (KeySym)ch;
    } else {
      /* Latin-1 supplement and beyond: try the Unicode keysym convention. */
      sym = 0x01000000 | ch;
    }
    KeyCode code = XKeysymToKeycode(dpy, sym);
    if (!code) {
      g_warning("[input] no keycode for U+%04X, skipping", (unsigned)ch);
      any_failure = true;
      p = g_utf8_next_char(p);
      continue;
    }
    bool need_shift = (ch >= 'A' && ch <= 'Z');
    KeyCode shift = XKeysymToKeycode(dpy, XK_Shift_L);
    if (need_shift && shift) XTestFakeKeyEvent(dpy, shift, True, CurrentTime);
    XTestFakeKeyEvent(dpy, code, True, CurrentTime);
    XTestFakeKeyEvent(dpy, code, False, CurrentTime);
    if (need_shift && shift) XTestFakeKeyEvent(dpy, shift, False, CurrentTime);
    XFlush(dpy);
    /* Paced so the toolkit has time to commit one character before the next;
       a burst can drop or reorder characters in editors that process events
       lazily. */
    g_usleep(12000);
    p = g_utf8_next_char(p);
  }
  return !any_failure;
}

#else /* !CU_HAVE_X11 */

bool cu_x11_available(void) { return false; }
static bool x11_move_and_click(double x, double y, int b, int c) {
  (void)x; (void)y; (void)b; (void)c; return false;
}
static bool x11_drag(double a, double b, double c, double d) {
  (void)a; (void)b; (void)c; (void)d; return false;
}
static bool x11_scroll(double x, double y, int sx, int sy) {
  (void)x; (void)y; (void)sx; (void)sy; return false;
}
static bool x11_press_key(const char *key, int repeat) {
  (void)key; (void)repeat; return false;
}
static bool x11_type_text(const char *text) {
  (void)text; return false;
}

#endif /* CU_HAVE_X11 */

/* ============================== Wayland =================================== */

/*
 * The portal's Notify* methods live in wayland.c, next to the session state.
 * This section adapts them to the same shape the X11 functions have.
 *
 * Two behavioural differences from X11 worth knowing, neither of which changes
 * the public API:
 *
 *   - There is no pointer position to restore. Wayland delivers absolute
 *     positions relative to the stream, so the user's own pointer does not jump
 *     when we inject a click. That is better than XTest's move-and-restore.
 *   - Every call needs a live session, which may mean a consent dialog on the
 *     first injection of the process. Opening it lazily means an agent that
 *     never touches the desktop never prompts.
 */
/*
 * Lazily opens the RemoteDesktop session on the first injection.
 *
 * This is the one place where a Computer Use call can take two minutes, because
 * the compositor puts a security dialog on screen and waits for a human. That is
 * unavoidable — but it must not be silent, so the first attempt logs why we are
 * about to block. When it does fail, cu_input_last_error() carries a message the
 * MCP layer surfaces instead of a bare "input unavailable".
 */
static bool wayland_ensure_session(void) {
  if (cu_remote_desktop_active()) return true;
  g_debug("[input] no RemoteDesktop session yet; asking the compositor for consent");
  return cu_input_open_wayland_session();
}

/* Logical (compositor) coordinates, which is also what AT-SPI reports. */
static bool wl_click_at(double x, double y, int button, int click_count) {
  if (!wayland_ensure_session()) return false;
  if (!cu_rd_pointer_motion_absolute(x, y)) return false;
  for (int i = 0; i < MAX(1, click_count); i++) {
    if (!cu_rd_pointer_button(button, true)) return false;
    if (!cu_rd_pointer_button(button, false)) return false;
  }
  return true;
}

static bool wl_drag(double from_x, double from_y, double to_x, double to_y) {
  if (!wayland_ensure_session()) return false;
  if (!cu_rd_pointer_motion_absolute(from_x, from_y)) return false;
  if (!cu_rd_pointer_button(1, true)) return false;

  /* Intermediate motion is not optional: toolkits only begin a drag once they
     have seen motion while the button is held. */
  const int steps = 16;
  for (int i = 1; i <= steps; i++) {
    double t = (double)i / steps;
    cu_rd_pointer_motion_absolute(from_x + (to_x - from_x) * t,
                                  from_y + (to_y - from_y) * t);
    g_usleep(10000);
  }
  return cu_rd_pointer_button(1, false);
}

static bool wl_scroll(double x, double y, int scroll_x, int scroll_y) {
  if (!wayland_ensure_session()) return false;
  if (!cu_rd_pointer_motion_absolute(x, y)) return false;

  /* AxisDiscrete takes *steps*, not pixels. 50px per notch matches the X11
     backend so the same scroll delta moves the same distance on both. */
  int v = scroll_y / 50;
  int h = scroll_x / 50;
  bool ok = true;
  if (v != 0) ok = cu_rd_pointer_axis_discrete(false, v) && ok;
  if (h != 0) ok = cu_rd_pointer_axis_discrete(true, h) && ok;
  return ok;
}

/*
 * Named keys go through NotifyKeyboardKeysym, so no keymap translation is
 * needed and the result does not depend on the user's layout being evdev.
 */
static bool wl_press_key(const char *key, int repeat) {
  if (!key) return false;
  if (!wayland_ensure_session()) return false;

  gchar **parts = g_strsplit(key, "+", CU_MAX_KEYSTROKE);
  int n = g_strv_length(parts);
  if (n == 0) { g_strfreev(parts); return false; }

  unsigned long mods[CU_MAX_KEYSTROKE];
  int nmods = 0;
  for (int i = 0; i < n - 1; i++) {
    const char *name = g_strstrip(parts[i]);
    unsigned long sym;
    if (g_ascii_strcasecmp(name, "cmd") == 0 ||
        g_ascii_strcasecmp(name, "meta") == 0 ||
        g_ascii_strcasecmp(name, "super") == 0) {
      sym = 0xffeb;                        /* XK_Super_L */
    } else if (g_ascii_strcasecmp(name, "ctrl") == 0 ||
               g_ascii_strcasecmp(name, "control") == 0) {
      sym = 0xffe3;                        /* XK_Control_L */
    } else if (g_ascii_strcasecmp(name, "alt") == 0 ||
               g_ascii_strcasecmp(name, "option") == 0) {
      sym = 0xffe9;                        /* XK_Alt_L */
    } else if (g_ascii_strcasecmp(name, "shift") == 0) {
      sym = 0xffe1;                        /* XK_Shift_L */
    } else if (g_ascii_strcasecmp(name, "fn") == 0) {
      continue;                            /* no Linux equivalent */
    } else {
      sym = cu_keysym_from_name(name);
      if (!sym) continue;
    }
    if (nmods < CU_MAX_KEYSTROKE) mods[nmods++] = sym;
  }

  unsigned long final_sym = cu_keysym_from_name(g_strstrip(parts[n - 1]));
  g_strfreev(parts);
  if (!final_sym) return false;

  bool ok = true;
  for (int r = 0; r < MAX(1, repeat); r++) {
    bool mods_ok = true;
    for (int m = 0; m < nmods; m++) {
      if (!cu_rd_key_keysym(mods[m], true)) { mods_ok = false; break; }
    }
    if (mods_ok) g_usleep(8000);
    bool key_ok = mods_ok && cu_rd_key_keysym(final_sym, true)
                            && cu_rd_key_keysym(final_sym, false);
    /* Release the modifiers even if the key half failed: leaving a modifier
       stuck down would corrupt every following keystroke. */
    for (int m = nmods - 1; m >= 0; m--) cu_rd_key_keysym(mods[m], false);
    ok = ok && mods_ok && key_ok;
    if (repeat > 1) g_usleep(20000);
  }
  return ok;
}

/*
 * Typing arbitrary text on Wayland.
 *
 * X11 maps each character to a keysym and injects it; the portal's
 * NotifyKeyboardKeysym takes the same keysyms, so that mostly carries over. What
 * does not carry over is everything that has no keysym: CJK, emoji, most of
 * Unicode. There is no way around it through this interface — it would take a
 * virtual keyboard carrying its own keymap (zwp_virtual_keyboard_v1, wlroots
 * only) or a text-input protocol, and the portal exposes neither.
 *
 * We type what can be typed and report that something could not be, rather than
 * silently dropping characters and leaving the model to wonder why the form it
 * filled in came out wrong.
 */
static bool wl_type_text(const char *text) {
  if (!text) return false;
  if (!wayland_ensure_session()) return false;

  /* Fast path: the EIS virtual text device commits the whole ASCII string in
     one call. We only take it for single-line ASCII: newlines are committed
     through the Return key on the per-key path below, and non-ASCII scripts
     are not committed over the XWayland/XIM path even when the text device is
     bound (they degrade to placeholder keys), so they fall through. */
  bool ascii_only = true;
  for (const char *p = text; *p; ) {
    gunichar ch = g_utf8_get_char(p);
    if (ch >= 0x80 || ch == '\n' || ch == '\r') { ascii_only = false; break; }
    p = g_utf8_next_char(p);
  }
  if (ascii_only && cu_rd_type_utf8(text)) return true;

  bool any_failure = false;
  for (const char *p = text; *p; ) {
    gunichar ch = g_utf8_get_char(p);
    if (ch == '\n' || ch == '\r') {
      wl_press_key("return", 1);
      p = g_utf8_next_char(p);
      continue;
    }

    unsigned long sym;
    bool need_shift = false;
    if (ch < 0x100) {
      /* Printable Latin-1: the keysym equals the code point, except capitals,
         which live on the shift level of the lowercase key. */
      if (ch >= 'A' && ch <= 'Z') {
        sym = (unsigned long)g_ascii_tolower((int)ch);
        need_shift = true;
      } else {
        sym = (unsigned long)ch;
      }
    } else if (ch < 0x10000) {
      /* Unicode keysyms are 0x01000000 | codepoint: Cyrillic, Greek, Latin-2
         and the rest of the BMP work this way. */
      sym = 0x01000000UL | (unsigned long)ch;
    } else {
      g_warning("[input] U+%04X has no keysym; not typable through the portal",
                (unsigned)ch);
      any_failure = true;
      p = g_utf8_next_char(p);
      continue;
    }

    if (need_shift) cu_rd_key_keysym(0xffe1 /* Shift_L */, true);
    bool ok = cu_rd_key_keysym(sym, true) && cu_rd_key_keysym(sym, false);
    if (need_shift) cu_rd_key_keysym(0xffe1, false);
    if (!ok) any_failure = true;
    /* Paced like the X11 path: one settled character before the next. */
    g_usleep(12000);
    p = g_utf8_next_char(p);
  }
  return !any_failure;
}

/* ============================== dispatch ================================== */

/*
 * Backend selection.
 *
 * On X11, XTest wins outright: no consent, no revocation mid-turn, reaches every
 * X client.
 *
 * On Wayland there is a decision worth getting right. XTest still "works" under
 * XWayland, but only for X11 clients — a native Wayland app never sees those
 * events. We therefore do NOT fall back blindly: the RemoteDesktop portal is
 * tried first, and XTest is used afterwards only when the caller resolved a
 * concrete X11 window handle (x11_window != 0) and an X display is reachable.
 * Without that handle, a fallback would report success while delivering the
 * event to whatever happens to be focused — possibly the wrong window — so we
 * fail and say why instead.
 */
bool cu_input_click_at(double x, double y, int button, int click_count,
                       unsigned long x11_window) {
  if (prefer_wayland_input()) {
    if (wl_click_at(x, y, button, click_count)) return true;
    if (x11_window != 0 && cu_x11_available()) {
      return x11_move_and_click(x, y, button, click_count);
    }
    return false;
  }
  return x11_move_and_click(x, y, button, click_count);
}

bool cu_input_drag(double from_x, double from_y, double to_x, double to_y,
                   unsigned long x11_window) {
  if (prefer_wayland_input()) {
    if (wl_drag(from_x, from_y, to_x, to_y)) return true;
    if (x11_window != 0 && cu_x11_available()) {
      return x11_drag(from_x, from_y, to_x, to_y);
    }
    return false;
  }
  return x11_drag(from_x, from_y, to_x, to_y);
}

bool cu_input_scroll_at(double x, double y, int scroll_x, int scroll_y,
                        unsigned long x11_window) {
  if (prefer_wayland_input()) {
    if (wl_scroll(x, y, scroll_x, scroll_y)) return true;
    if (x11_window != 0 && cu_x11_available()) {
      return x11_scroll(x, y, scroll_x, scroll_y);
    }
    return false;
  }
  return x11_scroll(x, y, scroll_x, scroll_y);
}

bool cu_input_press_key(const char *key, int repeat, unsigned long x11_window) {
  if (prefer_wayland_input()) {
    if (wl_press_key(key, repeat)) return true;
    if (x11_window != 0 && cu_x11_available()) return x11_press_key(key, repeat);
    return false;
  }
  return x11_press_key(key, repeat);
}

bool cu_input_type_text(const char *text, unsigned long x11_window) {
  if (prefer_wayland_input()) {
    if (wl_type_text(text)) return true;
    if (x11_window != 0 && cu_x11_available()) return x11_type_text(text);
    return false;
  }
  return x11_type_text(text);
}

/* ---------------------------- backend reporting --------------------------- */

const char *cu_input_preferred_backend(void) {
  const char *session = cu_session_type();
  if (strcmp(session, "x11") == 0) return "xtest";
  if (cu_remote_desktop_active()) return "remote-desktop";
  if (cu_wlroots_likely()) return "wlroots-virtual-input";
  if (strcmp(session, "wayland") == 0) return "remote-desktop-pending-consent";
  return cu_x11_available() ? "xtest" : "none";
}

/* --------------------------------- checks --------------------------------- */

char *cu_permissions_status(void) {
  CuDesktop de;
  cu_desktop_detect(&de);

  CuJson j; cu_json_init(&j);
  cu_json_obj_begin(&j);
  cu_json_key(&j, "platform"); cu_json_str(&j, "linux");
  cu_json_key(&j, "sessionType"); cu_json_str(&j, de.session_type);
  cu_json_key(&j, "desktop"); cu_json_str(&j, de.name);

  /* accessibility: AT-SPI reads are not permission-gated on Linux, so we report
     whether the tree is actually reachable and whether apps are publishing
     themselves to it — the second is the failure people actually hit. */
  cu_json_key(&j, "accessibility");
  cu_json_obj_begin(&j);
  bool atspi = cu_atspi_bus_reachable();
  cu_json_key(&j, "status");
  cu_json_str(&j, !atspi ? "missing" : (de.toolkit_accessibility ? "granted" : "limited"));
  cu_json_key(&j, "detail");
  if (!atspi) {
    cu_json_str(&j, "AT-SPI2 is not reachable. Install at-spi2-core and make sure "
                    "the accessibility bus is running.");
  } else if (!de.toolkit_accessibility) {
    cu_json_str(&j, "GNOME's toolkit-accessibility setting is off, so GTK "
                    "applications do not publish themselves to the tree. Run "
                    "`gsettings set org.gnome.desktop.interface "
                    "toolkit-accessibility true` and restart the app.");
  } else {
    cu_json_str(&j, "AT-SPI2 accessibility tree is readable.");
  }
  cu_json_obj_end(&j);

  /* screenRecording: the honest answer depends on whether the compositor can be
     captured silently, gated, or only through a consent dialog. */
  cu_json_key(&j, "screenRecording");
  cu_json_obj_begin(&j);
  const char *cap_backend = cu_capture_preferred_backend();
  cu_json_key(&j, "backend"); cu_json_str(&j, cap_backend);
  if (de.has_x11 && !de.is_wayland) {
    cu_json_key(&j, "status"); cu_json_str(&j, "granted");
    cu_json_key(&j, "detail");
    cu_json_str(&j, "X11 window capture is available without a prompt.");
  } else if (de.gnome_shell_screenshot) {
    cu_json_key(&j, "status"); cu_json_str(&j, "granted");
    cu_json_key(&j, "detail");
    cu_json_str(&j, "GNOME Shell's screenshot interface captures silently on both "
                    "X11 and Wayland.");
  } else if (cu_session_is_wayland() &&
             cu_bus_name_has_owner("org.kde.KWin.ScreenShot2")) {
    cu_json_key(&j, "status"); cu_json_str(&j, "granted");
    cu_json_key(&j, "detail");
    cu_json_str(&j, "KWin's ScreenShot2 restricted interface captures silently on "
                    "KDE Wayland; authorization is granted through the desktop "
                    "whitelist in the packaged .desktop file.");
  } else if (de.portal_screenshot_version > 0) {
    cu_json_key(&j, "status"); cu_json_str(&j, "prompt-required");
    cu_json_key(&j, "detail");
    cu_json_str(&j, "Wayland has no silent capture. The compositor asks for "
                    "confirmation through xdg-desktop-portal; once the grant is "
                    "remembered it answers without a dialog.");
  } else {
    cu_json_key(&j, "status"); cu_json_str(&j, "missing");
    cu_json_key(&j, "detail");
    cu_json_str(&j, "No screenshot backend is available: no X11 display, no "
                    "screenshot portal, and no desktop-specific interface.");
  }
  cu_json_obj_end(&j);

  /* inputInjection */
  cu_json_key(&j, "inputInjection");
  cu_json_obj_begin(&j);
  cu_json_key(&j, "backend"); cu_json_str(&j, cu_input_preferred_backend());
  bool xtest = cu_x11_available();
  bool rd = de.portal_remote_desktop_version > 0;
  if (strcmp(de.session_type, "x11") == 0 && xtest) {
    cu_json_key(&j, "status"); cu_json_str(&j, "granted");
    cu_json_key(&j, "detail");
    cu_json_str(&j, "XTest (XTEST extension) can synthesize input.");
  } else if (cu_remote_desktop_active()) {
    cu_json_key(&j, "status"); cu_json_str(&j, "granted");
    cu_json_key(&j, "detail");
    cu_json_str(&j, "A RemoteDesktop portal session is active; input is injected "
                    "through the compositor.");
  } else if (rd) {
    cu_json_key(&j, "status"); cu_json_str(&j, "prompt-required");
    cu_json_key(&j, "detail");
    cu_json_str(&j, "Wayland forbids one client from synthesizing input for "
                    "another. The RemoteDesktop portal can grant it, and the "
                    "compositor will ask for confirmation the first time input "
                    "is needed.");
  } else if (xtest) {
    cu_json_key(&j, "status"); cu_json_str(&j, "limited");
    cu_json_key(&j, "detail");
    cu_json_str(&j, "Only XTest is available, which reaches XWayland clients but "
                    "not native Wayland ones.");
  } else {
    cu_json_key(&j, "status"); cu_json_str(&j, "unsupported");
    cu_json_key(&j, "detail");
    cu_json_str(&j, "No input backend is usable on this session.");
  }
  cu_json_obj_end(&j);

  if (cu_input_last_error()) {
    cu_json_key(&j, "inputError"); cu_json_str(&j, cu_input_last_error());
  }
  if (cu_capture_last_error()) {
    cu_json_key(&j, "captureError"); cu_json_str(&j, cu_capture_last_error());
  }

  /* ready: can the caller do useful work at all? Capability per verb is in the
     three blocks above; this is the single boolean most callers want. */
  bool ready = atspi && (xtest || rd || de.gnome_shell_screenshot);
  cu_json_key(&j, "ready"); cu_json_bool(&j, ready);
  cu_json_obj_end(&j);

  char *out = g_strdup(j.buf->str);
  cu_json_free(&j);
  g_free(de.x11_wm_name);
  return out;
}

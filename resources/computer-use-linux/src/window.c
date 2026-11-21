/*
 * Window activation and closing on Linux.
 *
 * This is the one Computer Use verb with no portable answer, because the two
 * session protocols took opposite positions on it:
 *
 *   X11:      Focus and lifetime are the *window manager's* business, and EWMH
 *             gives any client a standard way to ask. _NET_ACTIVE_WINDOW
 *             carries a source indication so the WM can tell a pager or a tool
 *             from a genuine user action, and _NET_CLOSE_WINDOW asks for a
 *             close the way clicking the titlebar button does. Every compliant
 *             WM implements both — kwin, mutter, xfwm4, marco, openbox alike,
 *             so one code path covers KDE, GNOME, XFCE, MATE and Cinnamon in
 *             their X11 sessions.
 *
 *   Wayland:  the compositor owns focus and deliberately refuses requests from
 *             ordinary clients — the whole point is that a background app can
 *             not steal focus or close your window. There is no protocol for
 *             "focus that other window", so we do not pretend there is. Two
 *             honest options remain:
 *               - AT-SPI Component.grab_focus on the frame, which some
 *                 toolkits forward to the compositor (Qt does; KWin generally
 *                 accepts it from its own clients, Mutter does not).
 *               - Give up and say so, so the model can ask the user to click.
 *             We do not fake it by injecting a click, which would land on
 *             whatever happens to be on top.
 */
#include "cu.h"

#include <string.h>

#if defined(CU_HAVE_X11)
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#endif

/* ------------------------------------------------------------------------- */
/*                                WM probing                                 */
/* ------------------------------------------------------------------------- */

#if defined(CU_HAVE_X11)

/*
 * Xlib's default error handler calls exit() on a protocol error, which would
 * take the whole JSON-RPC session down with it. Every probing call below is
 * wrapped in this so a dead WM or a vanished window degrades to "no answer".
 */
static int g_wm_probe_error = 0;
static int wm_probe_error_handler(Display *dpy, XErrorEvent *ev) {
  (void)dpy;
  g_wm_probe_error = ev->error_code;
  return 0;
}

/*
 * Reads a single 32-bit property.
 *
 * The type matters and is a classic trap: _NET_SUPPORTING_WM_CHECK is declared
 * as WINDOW (XA_WINDOW), while _NET_ACTIVE_WINDOW is also WINDOW, but
 * _NET_CURRENT_DESKTOP is CARDINAL. Asking XGetWindowProperty for the wrong type
 * returns the data but with actual_format 0 / actual_type mismatched — or, when
 * we pass the wrong type filter, nothing at all. So the expected type is a
 * parameter rather than assumed.
 */
static bool get_32bit_prop(Display *dpy, Window win, Atom prop, Atom expected_type,
                           unsigned long *out) {
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long n_items = 0, bytes_after = 0;
  unsigned char *data = NULL;

  /*
   * Drain first, then reset. Xlib reports protocol errors asynchronously, so a
   * stale BadWindow from an earlier probe can still be sitting in the queue and
   * would be delivered *during* this call — landing in g_wm_probe_error and
   * making a perfectly good read look like a failure. XSync before clearing is
   * what makes the flag mean "an error from this request".
   */
  XSync(dpy, False);
  g_wm_probe_error = 0;
  int rc = XGetWindowProperty(dpy, win, prop, 0, 1, False, expected_type,
                              &actual_type, &actual_format, &n_items,
                              &bytes_after, &data);
  /* Push the reply and any error through before the flag is inspected. */
  XSync(dpy, False);
  if (rc != Success || g_wm_probe_error || !data || actual_format != 32 ||
      n_items < 1 || actual_type != expected_type) {
    if (data) XFree(data);
    return false;
  }
  *out = ((unsigned long *)data)[0];
  XFree(data);
  return true;
}

/* Reads a window property as a UTF-8 string (WM_NAME / _NET_WM_NAME). */
static char *get_utf8_prop(Display *dpy, Window win, Atom prop) {
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long n_items = 0, bytes_after = 0;
  unsigned char *data = NULL;

  XSync(dpy, False);
  g_wm_probe_error = 0;
  int rc = XGetWindowProperty(dpy, win, prop, 0, 256, False, AnyPropertyType,
                              &actual_type, &actual_format, &n_items,
                              &bytes_after, &data);
  XSync(dpy, False);
  if (rc != Success || g_wm_probe_error || !data) {
    if (data) XFree(data);
    return NULL;
  }
  char *out = g_strndup((const char *)data, n_items);
  XFree(data);
  return out;
}

void cu_x11_detect_wm(CuDesktop *out) {
  Display *dpy = XOpenDisplay(NULL);
  if (!dpy) return;

  XErrorHandler previous = XSetErrorHandler(wm_probe_error_handler);
  Window root = DefaultRootWindow(dpy);

  Atom net_supporting = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", True);
  Atom net_wm_name_a = XInternAtom(dpy, "_NET_WM_NAME", True);
  Atom utf8 = XInternAtom(dpy, "UTF8_STRING", True);

  /* The EWMH handshake: the root window points at a child that names the WM,
     and that child points at itself (the self-reference defeats recycled ids). */
  if (net_supporting != None) {
    unsigned long wm_win = 0;
    /* _NET_SUPPORTING_WM_CHECK is a WINDOW property, not a CARDINAL. */
    if (get_32bit_prop(dpy, root, net_supporting, XA_WINDOW, &wm_win) && wm_win != 0) {
      g_wm_probe_error = 0;
      XWindowAttributes attr;
      if (XGetWindowAttributes(dpy, (Window)wm_win, &attr)) {
        XSync(dpy, False);
        if (!g_wm_probe_error) {
          out->x11_ewmh = true;
          if (net_wm_name_a != None) {
            out->x11_wm_name = get_utf8_prop(dpy, (Window)wm_win, net_wm_name_a);
          }
          if (!out->x11_wm_name) {
            out->x11_wm_name = get_utf8_prop(dpy, (Window)wm_win, XA_WM_NAME);
          }
          (void)utf8;
        }
      }
    }
  }

  /* Which EWMH requests this WM actually honours. We only claim the ones the
     spec says every compliant WM must support, and only when _NET_SUPPORTED
     lists them — a WM that does not implement close would otherwise swallow
     the message and leave the caller thinking it worked. */
  Atom net_supported = XInternAtom(dpy, "_NET_SUPPORTED", True);
  if (net_supported != None) {
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long n = 0, after = 0;
    unsigned char *data = NULL;
    g_wm_probe_error = 0;
    int rc = XGetWindowProperty(dpy, root, net_supported, 0, 1024, False, XA_ATOM,
                                &actual_type, &actual_format, &n, &after, &data);
    if (rc == Success && !g_wm_probe_error && data && actual_format == 32) {
      Atom wanted_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", True);
      Atom wanted_close = XInternAtom(dpy, "_NET_CLOSE_WINDOW", True);
      Atom *atoms = (Atom *)data;
      for (unsigned long i = 0; i < n; i++) {
        if (atoms[i] == wanted_active) out->x11_has_active_window = true;
        if (atoms[i] == wanted_close) out->x11_has_close_window = true;
      }
    }
    if (data) XFree(data);
  }

  XSetErrorHandler(previous);
  XCloseDisplay(dpy);
}

#else

void cu_x11_detect_wm(CuDesktop *out) {
  (void)out;
}

#endif /* CU_HAVE_X11 */

/* ------------------------------------------------------------------------- */
/*                            EWMH client messages                           */
/* ------------------------------------------------------------------------- */

#if defined(CU_HAVE_X11)

/*
 * Sends one EWMH client message to the root window.
 *
 * data.l[0] is the source indication: 2 means "pager or tool". That is the
 * honest value for an automation helper, and it lets the WM apply its
 * focus-stealing-prevention policy (some flash the taskbar entry instead of
 * raising) rather than treating us as the user. Passing 0 or 1 would make the
 * request look like a direct user action, which would bypass those guards —
 * convenient, but it is exactly the behaviour WMs are protecting the user from.
 */
static bool send_ewmh_message(Window win, const char *message, long l1, long l2) {
  if (win == 0) return false;
  Display *dpy = XOpenDisplay(NULL);
  if (!dpy) return false;

  Atom type = XInternAtom(dpy, message, True);
  if (type == None) {
    XCloseDisplay(dpy);
    return false;
  }

  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.xclient.type = ClientMessage;
  ev.xclient.serial = 0;
  ev.xclient.send_event = True;
  ev.xclient.display = dpy;
  ev.xclient.window = win;
  ev.xclient.message_type = type;
  ev.xclient.format = 32;
  ev.xclient.data.l[0] = 2;              /* source indication: pager/tool */
  ev.xclient.data.l[1] = l1;             /* timestamp, or 0 for close */
  ev.xclient.data.l[2] = l2;

  XErrorHandler previous = XSetErrorHandler(wm_probe_error_handler);
  g_wm_probe_error = 0;
  XSendEvent(dpy, DefaultRootWindow(dpy), False,
             SubstructureRedirectMask | SubstructureNotifyMask, &ev);
  XSync(dpy, False);
  XSetErrorHandler(previous);

  bool ok = (g_wm_probe_error == 0);
  XCloseDisplay(dpy);
  return ok;
}

/* EWMH activation, the portable X11 route. */
bool cu_x11_activate(unsigned long x11_window) {
  /* Timestamp 0 = "no user event behind this", which WMs treat as a
     lower-priority request rather than an outright refusal. */
  return send_ewmh_message((Window)x11_window, "_NET_ACTIVE_WINDOW", CurrentTime, 0);
}

/*
 * _NET_CLOSE_WINDOW: the WM forwards WM_DELETE_WINDOW to the client, which is
 * the same thing clicking the titlebar X does. The app still decides — an
 * editor with unsaved changes will prompt instead of exiting, which is correct
 * and is why this is a request rather than a kill.
 */
bool cu_x11_close(unsigned long x11_window) {
  return send_ewmh_message((Window)x11_window, "_NET_CLOSE_WINDOW", CurrentTime, 2);
}

/*
 * Finds the X11 window that corresponds to an AT-SPI window.
 *
 * The two id spaces are unrelated: AT-SPI hands out its own arbitrary ids.
 * Without a mapping, an EWMH-capable session would still end up using the
 * toolkit path for every window, which is exactly the weaker route we are
 * trying to prefer.
 *
 * Matching is a scoring problem, not an equality test, because the two sides
 * disagree about what identifies an app:
 *
 *   - Native Wayland clients (Qt6, GTK4) do not appear in _NET_CLIENT_LIST at
 *     all. Their AT-SPI window has a pid but no X window exists; a pid match
 *     simply finds nothing and we correctly fall back to AT-SPI.
 *   - XWayland clients do appear, but their _NET_WM_PID is often absent or
 *     belongs to a nested helper (the observed kate window under KWin reports
 *     pid 16213, WorkBuddy's pid, because it is not an X11 client at all).
 *   - The title can change between list_windows and the action, and a
 *     single-window app is usually titled "<doc> — Kate" while AT-SPI reports
 *     the frame title verbatim. Exact equality is too brittle.
 *
 * So candidates come from two independent sources — pid, and WM_CLASS matched
 * against the desktop entry's StartupWMClass/app id/exec basename — and the
 * title only ranks them. Nothing matches on title alone: closing the wrong
 * window is far worse than falling back to AT-SPI.
 *
 * Returns 0 when nothing matches, which every caller treats as "use AT-SPI".
 */
static bool wm_class_matches(Display *dpy, Window w, GPtrArray *wanted) {
  XClassHint hint;
  memset(&hint, 0, sizeof(hint));
  XSync(dpy, False);
  g_wm_probe_error = 0;
  bool ok = XGetClassHint(dpy, w, &hint) != 0;
  XSync(dpy, False);
  if (g_wm_probe_error) ok = false;
  bool matched = false;
  if (ok) {
    const char *values[2] = { hint.res_name, hint.res_class };
    for (int v = 0; v < 2 && !matched; v++) {
      if (!values[v] || !*values[v]) continue;
      char *lower = g_ascii_strdown(values[v], -1);
      for (guint i = 0; i < wanted->len && !matched; i++) {
        const char *cand = g_ptr_array_index(wanted, i);
        if (cand && (strcmp(lower, cand) == 0 ||
                     strstr(cand, lower) != NULL ||
                     strstr(lower, cand) != NULL)) {
          matched = true;
        }
      }
      g_free(lower);
    }
    if (hint.res_name) XFree(hint.res_name);
    if (hint.res_class) XFree(hint.res_class);
  }
  return matched;
}

static char *x11_title(Display *dpy, Window w, Atom net_wm_name_a) {
  char *t = NULL;
  if (net_wm_name_a != None) t = get_utf8_prop(dpy, w, net_wm_name_a);
  if (!t) t = get_utf8_prop(dpy, w, XA_WM_NAME);
  return t;
}

/*
 * Scores one window as a match for (pid, title, WM_CLASS candidates).
 *
 * Returns 0 for "not a candidate". Title alone never qualifies a window —
 * closing or raising the wrong one is worse than giving up and letting the
 * caller fall back to AT-SPI.
 */
static int score_window(Display *dpy, Window w, Atom net_wm_pid,
                        Atom net_wm_name_a, int pid, const char *title,
                        GPtrArray *wanted) {
  bool pid_match = false;
  if (pid > 0 && net_wm_pid != None) {
    unsigned long wpid = 0;
    if (get_32bit_prop(dpy, w, net_wm_pid, XA_CARDINAL, &wpid) &&
        (int)wpid == pid) {
      pid_match = true;
    }
  }
  bool class_match = wanted->len > 0 && wm_class_matches(dpy, w, wanted);
  if (!pid_match && !class_match) return 0;

  /*
   * A class match on an unmapped or iconified window is not a usable target:
   * EWMH activation is a request to the WM, and the WM can only honour it for a
   * window it manages and can map. awesun on this machine keeps several
   * never-managed helper windows with the right WM_CLASS; raising one of those
   * succeeds at the protocol level and does visibly nothing, which is the worst
   * outcome because the caller believes it worked.
   *
   * Checked only for class matches. A pid match is a statement about identity,
   * and a pid-matched window that is merely minimised is exactly what
   * activation is supposed to restore.
   */
  if (!pid_match) {
    XWindowAttributes attr;
    XSync(dpy, False);
    g_wm_probe_error = 0;
    bool got = XGetWindowAttributes(dpy, w, &attr) != 0;
    XSync(dpy, False);
    if (!got || g_wm_probe_error || attr.map_state == IsUnmapped) return 0;
  }

  int score = 0;
  if (pid_match) score += 40;
  if (class_match) score += 20;
  if (title && *title) {
    char *wt = x11_title(dpy, w, net_wm_name_a);
    if (wt) {
      if (strcmp(wt, title) == 0) score += 100;
      else if (*wt && (strstr(title, wt) || strstr(wt, title))) score += 30;
      g_free(wt);
    }
  }
  return score;
}

/*
 * Depth-limited walk of the window tree.
 *
 * _NET_CLIENT_LIST is the documented source and is tried first, but it is not
 * trustworthy in practice: under KWin on this machine it listed one window
 * while the root had 30 children, including a mapped, currently running
 * application (awesun) that was demonstrably absent from the list. A window
 * manager is only obliged to maintain the list for windows it manages as
 * clients; nested and never-framed windows fall outside it.
 *
 * So when the list yields nothing we walk the tree. Depth stays small: managed
 * frames sit at depth 1-2, and walking deeper would let a match inside an
 * unrelated app's UI win.
 */
static void walk_for_match(Display *dpy, Window w, int depth,
                           Atom net_wm_pid, Atom net_wm_name_a,
                           int pid, const char *title, GPtrArray *wanted,
                           Window *best, int *best_score) {
  if (depth > 2) return;

  Window root = 0, parent = 0, *kids = NULL;
  unsigned n = 0;
  if (!XQueryTree(dpy, w, &root, &parent, &kids, &n)) return;

  for (unsigned i = 0; i < n; i++) {
    Window child = kids[i];
    int score = score_window(dpy, child, net_wm_pid, net_wm_name_a,
                             pid, title, wanted);
    if (score > *best_score) {
      *best_score = score;
      *best = child;
    }
    walk_for_match(dpy, child, depth + 1, net_wm_pid, net_wm_name_a,
                   pid, title, wanted, best, best_score);
  }
  if (kids) XFree(kids);
}

unsigned long cu_x11_find_window(int pid, const char *title, const char *wm_class_query) {
  Display *dpy = XOpenDisplay(NULL);
  if (!dpy) return 0;

  XErrorHandler previous = XSetErrorHandler(wm_probe_error_handler);

  Atom net_client_list = XInternAtom(dpy, "_NET_CLIENT_LIST", True);
  Atom net_wm_pid = XInternAtom(dpy, "_NET_WM_PID", True);
  Atom net_wm_name_a = XInternAtom(dpy, "_NET_WM_NAME", True);

  GPtrArray *wanted = g_ptr_array_new_with_free_func(g_free);
  if (wm_class_query && *wm_class_query) {
    cu_desktop_wm_class_candidates(wm_class_query, wanted);
  }

  Window best = 0;
  int best_score = 0;

  if (net_client_list != None) {
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long n = 0, after = 0;
    unsigned char *data = NULL;
    XSync(dpy, False);
    g_wm_probe_error = 0;
    int rc = XGetWindowProperty(dpy, DefaultRootWindow(dpy), net_client_list, 0, 4096,
                                False, XA_WINDOW, &actual_type, &actual_format,
                                &n, &after, &data);
    XSync(dpy, False);
    if (rc == Success && !g_wm_probe_error && data && actual_format == 32) {
      Window *wins = (Window *)data;
      for (unsigned long i = 0; i < n; i++) {
        int score = score_window(dpy, wins[i], net_wm_pid, net_wm_name_a,
                                 pid, title, wanted);
        if (score > best_score) {
          best_score = score;
          best = wins[i];
        }
      }
    }
    if (data) XFree(data);
  }

  if (best == 0) {
    walk_for_match(dpy, DefaultRootWindow(dpy), 0, net_wm_pid, net_wm_name_a,
                   pid, title, wanted, &best, &best_score);
  }

  g_ptr_array_free(wanted, TRUE);
  XSetErrorHandler(previous);
  XCloseDisplay(dpy);
  return (unsigned long)best;
}

/*
 * Backwards-compatible wrapper for callers that only have a pid and title.
 * Kept because it reads better at the call site than passing an empty class.
 */
unsigned long cu_x11_find_window_by_pid_title(int pid, const char *title) {
  return cu_x11_find_window(pid, title, NULL);
}

/*
 * Reads back what an X window claims to be. Used to tell the caller which
 * window an EWMH request is about to hit — the difference between "I activated
 * kate" and "I activated something, I am not sure what".
 */
bool cu_x11_window_identity(unsigned long x11_window, int *pid, char **title) {
  if (x11_window == 0) return false;
  Display *dpy = XOpenDisplay(NULL);
  if (!dpy) return false;

  XErrorHandler previous = XSetErrorHandler(wm_probe_error_handler);
  Window w = (Window)x11_window;

  bool ok = false;
  g_wm_probe_error = 0;
  XWindowAttributes attr;
  if (XGetWindowAttributes(dpy, w, &attr)) {
    XSync(dpy, False);
    ok = (g_wm_probe_error == 0);
  }
  if (ok && pid) {
    Atom net_wm_pid = XInternAtom(dpy, "_NET_WM_PID", True);
    unsigned long value = 0;
    *pid = (net_wm_pid != None &&
            get_32bit_prop(dpy, w, net_wm_pid, XA_CARDINAL, &value))
      ? (int)value : 0;
  }
  if (ok && title) {
    Atom net_wm_name_a = XInternAtom(dpy, "_NET_WM_NAME", True);
    *title = net_wm_name_a != None ? get_utf8_prop(dpy, w, net_wm_name_a) : NULL;
    if (!*title) *title = get_utf8_prop(dpy, w, XA_WM_NAME);
  }

  XSetErrorHandler(previous);
  XCloseDisplay(dpy);
  return ok;
}

/* The focused top-level window, or 0 when the WM does not publish it. */
unsigned long cu_x11_active_window(void) {
  Display *dpy = XOpenDisplay(NULL);
  if (!dpy) return 0;

  Atom net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", True);
  unsigned long win = 0;
  if (net_active != None) {
    XErrorHandler previous = XSetErrorHandler(wm_probe_error_handler);
    /* _NET_ACTIVE_WINDOW is also a WINDOW, not a CARDINAL. */
    get_32bit_prop(dpy, DefaultRootWindow(dpy), net_active, XA_WINDOW, &win);
    XSetErrorHandler(previous);
  }
  XCloseDisplay(dpy);
  return win;
}

#else

bool cu_x11_activate(unsigned long x11_window) {
  (void)x11_window;
  return false;
}

bool cu_x11_close(unsigned long x11_window) {
  (void)x11_window;
  return false;
}

unsigned long cu_x11_find_window(int pid, const char *title, const char *wm_class_query) {
  (void)pid;
  (void)title;
  (void)wm_class_query;
  return 0;
}

unsigned long cu_x11_find_window_by_pid_title(int pid, const char *title) {
  (void)pid;
  (void)title;
  return 0;
}

bool cu_x11_window_identity(unsigned long x11_window, int *pid, char **title) {
  (void)x11_window;
  (void)pid;
  (void)title;
  return false;
}

unsigned long cu_x11_active_window(void) {
  return 0;
}

#endif /* CU_HAVE_X11 */

/*
 * XFCE-specific integration.
 *
 * XFCE is the simplest of the three mainstream desktops to support, for a
 * slightly awkward reason: it has no native Wayland session. As of XFCE 4.20
 * the Wayland port (xfwl4 / libxfce4windowing on wlroots) is experimental and
 * not what anyone runs; the shipping desktop is X11 with xfwm4 as the window
 * manager. That means all the good X11 paths apply unchanged:
 *
 *   - XTest injects input with no permission model.
 *   - xfwm4 implements EWMH, so _NET_ACTIVE_WINDOW and _NET_CLOSE_WINDOW work,
 *     and XGetImage on a window generally succeeds because xfwm4 does not
 *     compositor-redirect every client (its compositor is optional and, even
 *     when enabled, does not behave like Mutter's).
 *   - AT-SPI works as soon as at-spi2-core is running; XFCE has no equivalent
 *     of GNOME's toolkit-accessibility gate.
 *
 * The one place XFCE is genuinely behind is screen capture: xdg-desktop-portal
 * has no XFCE backend, so the portal falls through to -gtk (present, but with a
 * file chooser and no area selection) and, historically, Screenshot was simply
 * unimplemented there. xfce4-screenshooter is the reliable path, and it takes
 * an output *directory* plus options rather than a filename, which is why the
 * wrapper below has to rename what it produces.
 *
 * XFCE also gives us a genuinely useful primitive nobody else does: the window
 * manager can be asked to activate a window by *client* id, and xfce4-session
 * publishes the running session. We use neither — EWMH is more portable across
 * XFCE versions — but the screenshooter wrapper is load-bearing.
 */
#include "cu.h"

#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* ----------------------------- capture backend ---------------------------- */

/*
 * xfce4-screenshooter invocation:
 *
 *   xfce4-screenshooter -f                      full screen, save dialog
 *   xfce4-screenshooter -f -s <dir>             full screen, silent, into <dir>
 *   xfce4-screenshooter -w -s <dir>             active window
 *   xfce4-screenshooter -r -s <dir>             selected region
 *   xfce4-screenshooter -f -s <dir> -o <cmd>    hand the file path to <cmd>
 *
 * `-o` looks tempting but is not usable from a non-interactive caller: it
 * passes the path as one extra argv entry to <cmd>, so `-o cp` would run
 * `cp <file>` with no destination and fail. Its intended use is a helper script.
 * We therefore use `-s <dir>` (silent save into a directory we create) and then
 * pick up whatever PNG appeared there — the name is timestamped with the
 * locale's format, so predicting it is not an option.
 *
 * There is no area option that skips the interactive selection dialog, so `-r`
 * is only useful when a human is present. We therefore only ever ask for a
 * full-screen capture and crop to the AT-SPI rect ourselves — the same strategy
 * the KDE path uses, and for the same reason: the model already gave us a rect.
 */
char *cu_xfce_screenshot_to_file(int x, int y, int w, int h) {
  (void)x; (void)y; (void)w; (void)h;
  if (!cu_have_command("xfce4-screenshooter")) return NULL;

  char *dir = g_dir_make_tmp("lobster-cu-xfce-XXXXXX", NULL);
  if (!dir) return NULL;

  char *out_path = g_build_filename(dir, "capture.png", NULL);

  /* `-f` captures the whole screen; we crop afterwards. Asking for the active
     window would be wrong here: the window we were asked about is often not
     the focused one, and xfce4-screenshooter resolves "active" from the WM's
     focus, not from our AT-SPI rect. */
  char *argv[] = {
    (char *)"xfce4-screenshooter",
    (char *)"-f",
    (char *)"-s", dir,
    NULL
  };
  GError *err = NULL;
  gint status = 0;
  gboolean spawned = g_spawn_sync(NULL, argv, NULL,
                                  G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                                      G_SPAWN_STDERR_TO_DEV_NULL,
                                  NULL, NULL, NULL, NULL, &status, &err);
  if (!spawned) {
    if (err) g_error_free(err);
    g_free(out_path);
    g_free(dir);
    return NULL;
  }
  if (!g_spawn_check_wait_status(status, NULL)) {
    g_free(out_path);
    rmdir(dir);
    g_free(dir);
    return NULL;
  }

  /*
   * xfce4-screenshooter names the file itself (Screenshot_YYYY-MM-DD_HH-MM-SS
   * .png, or the locale's variant). Find the newest .png the run produced.
   */
  char *newest = NULL;
  GTimeSpan newest_age = 0;
  GDir *d = g_dir_open(dir, 0, NULL);
  if (d) {
    const char *name;
    while ((name = g_dir_read_name(d)) != NULL) {
      if (!g_str_has_suffix(name, ".png")) continue;
      char *full = g_build_filename(dir, name, NULL);
      struct stat st;
      if (stat(full, &st) == 0) {
        GTimeSpan age = (GTimeSpan)(g_get_real_time() - (gint64)st.st_mtime * 1000000);
        if (!newest || age < newest_age) {
          g_free(newest);
          newest = g_strdup(full);
          newest_age = age;
        }
      }
      g_free(full);
    }
    g_dir_close(d);
  }

  if (newest) {
    /* Move into the name the caller expects, so the crop step is uniform. */
    rename(newest, out_path);
    g_free(newest);
    rmdir(dir);
    g_free(dir);
    return out_path;
  }

  g_free(out_path);
  rmdir(dir);
  g_free(dir);
  return NULL;
}

/* ---------------------------- session reporting --------------------------- */

/*
 * XFCE's own session manager. Useful only as evidence: if xfce4-session is
 * owned, we are definitely in an XFCE session even when XDG_CURRENT_DESKTOP was
 * not inherited (which happens when the helper is launched from a systemd user
 * service that was started before the session environment was set up).
 */
bool cu_xfce_session_present(void) {
  return cu_bus_name_has_owner("org.xfce.SessionManager") ||
         cu_have_command("xfce4-session");
}

/*
 * A short capability note for XFCE, returned to the model in check_permissions
 * so it knows what to expect before it tries something that cannot work.
 */
char *cu_xfce_capability_note(void) {
  if (!cu_xfce_session_present()) return NULL;

  bool has_screenshooter = cu_have_command("xfce4-screenshooter");
  bool has_x11 = g_getenv("DISPLAY") != NULL;
  const char *session = cu_session_type();

  GString *note = g_string_new(NULL);
  if (strcmp(session, "wayland") == 0) {
    g_string_append(note,
        "XFCE has no native Wayland session; this looks like XWayland inside "
        "another compositor. X11 paths apply only to X11 clients.");
  } else if (has_x11) {
    g_string_append(note,
        "XFCE/X11 session: XTest input, EWMH window control and direct window "
        "capture are all available.");
    if (!has_screenshooter) {
      g_string_append(note,
          " xfce4-screenshooter is not installed, so full-screen capture falls "
          "back to the portal (which on XFCE is handled by the GTK backend and "
          "may be limited). Install xfce4-screenshooter for reliable capture.");
    }
  } else {
    g_string_append(note, "No X display found; XFCE needs one.");
  }
  return g_string_free(note, FALSE);
}

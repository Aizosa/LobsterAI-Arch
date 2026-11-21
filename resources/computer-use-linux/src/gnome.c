/*
 * GNOME-specific integration.
 *
 * GNOME is the desktop with the least tolerance for automation, and it is worth
 * being precise about which of its doors are open and which are nailed shut,
 * because they changed over time and half the advice online is stale.
 *
 *   org.gnome.Shell.Screenshot   — a *stable* D-Bus interface (it lives in
 *       gnome-shell's exported interface list, not in the private one) that
 *       captures the whole screen, a specific area, or the focused window to a
 *       PNG path we choose. It is silent and prompt-free. This is the best
 *       capture path on GNOME by a wide margin, on both X11 and Wayland, and we
 *       try it before the portal.
 *
 *   org.gnome.Shell.Eval        — executes JavaScript inside the shell process.
 *       Since GNOME 41 it is gated: on Wayland sessions the method is simply
 *       not exported, so a call fails with UnknownMethod even though the name
 *       org.gnome.Shell is owned. On X11 it is available to any session-bus
 *       client, which makes it a universal lever — raise windows, read the
 *       compositor's window list, even drive Shell UI. We probe for it rather
 *       than assuming, and we say which of the two failures we hit, because
 *       "not available on Wayland" is not an error the caller should retry.
 *
 *   org.gnome.Shell.Extensions  — the supported way to *install* a helper
 *       extension. We do not use it to run code; it matters only in that a
 *       "activate this window" workflow on GNOME Wayland genuinely needs one.
 *
 *   Accessibility opt-in        — GNOME ships toolkit-accessibility=false by
 *       default. With it off, GTK applications do not publish themselves to
 *       AT-SPI, so list_windows returns almost nothing and every element-level
 *       action fails. This is by far the most common reason Computer Use
 *       "doesn't work on GNOME", so we detect it and can turn it on.
 */
#include "cu.h"

#include <gio/gio.h>
#include <string.h>
#include <unistd.h>

static bool gnome_shell_present(void) {
  return cu_bus_name_has_owner("org.gnome.Shell");
}

/* ---------------------------- Eval availability --------------------------- */

/*
 * Three outcomes, and the caller needs to tell them apart:
 *   CU_GNOME_EVAL_OK          — reachable and a trivial expression evaluated
 *   CU_GNOME_EVAL_X11_ONLY    — shell present but the method is not exported
 *                               (GNOME >= 41 on Wayland): permanent, do not retry
 *   CU_GNOME_EVAL_ABSENT      — no GNOME Shell on this bus at all
 */
CuGnomeEvalState cu_gnome_eval_state(void) {
  if (!gnome_shell_present()) return CU_GNOME_EVAL_ABSENT;

  GError *err = NULL;
  GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!conn) {
    if (err) g_error_free(err);
    return CU_GNOME_EVAL_ABSENT;
  }
  GVariant *reply = g_dbus_connection_call_sync(
      conn, "org.gnome.Shell", "/org/gnome/Shell", "org.gnome.Shell", "Eval",
      g_variant_new("(s)", "1+1"), G_VARIANT_TYPE("(bs)"),
      G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err);
  g_object_unref(conn);

  if (reply) {
    g_variant_unref(reply);
    return CU_GNOME_EVAL_OK;
  }

  CuGnomeEvalState state = CU_GNOME_EVAL_ABSENT;
  if (err) {
    /* G_DBUS_ERROR_UNKNOWN_METHOD arrives as org.freedesktop.DBus.Error
       .UnknownMethod; some shells answer UnknownInterface instead. Both mean
       "the shell is here but will not evaluate for us". */
    if (g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD) ||
        g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE) ||
        g_dbus_error_is_remote_error(err)) {
      state = CU_GNOME_EVAL_X11_ONLY;
    }
    g_error_free(err);
  }
  return state;
}

const char *cu_gnome_eval_state_id(CuGnomeEvalState s) {
  switch (s) {
    case CU_GNOME_EVAL_OK:        return "available";
    case CU_GNOME_EVAL_X11_ONLY:  return "x11-only";
    default:                      return "absent";
  }
}

/*
 * Runs a snippet in the shell and returns its result expression.
 *
 * The shell returns (success, result) where `result` is the JS value coerced to
 * a string. Callers are given NULL on failure so they fall through to a
 * non-GNOME path rather than acting on a half-executed script.
 */
char *cu_gnome_eval(const char *script) {
  if (!script) return NULL;
  GError *err = NULL;
  GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!conn) {
    if (err) g_error_free(err);
    return NULL;
  }
  GVariant *reply = g_dbus_connection_call_sync(
      conn, "org.gnome.Shell", "/org/gnome/Shell", "org.gnome.Shell", "Eval",
      g_variant_new("(s)", script), G_VARIANT_TYPE("(bs)"),
      G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
  g_object_unref(conn);
  if (!reply) {
    if (err) g_error_free(err);
    return NULL;
  }

  gboolean ok = FALSE;
  const char *result = NULL;
  g_variant_get(reply, "(b&s)", &ok, &result);
  char *out = (ok && result) ? g_strdup(result) : NULL;
  g_variant_unref(reply);
  return out;
}

/*
 * Ask GNOME Shell to raise one of its windows, via the *supported* private
 * interface rather than Eval there is none — so this is Eval-based and only
 * works on X11 sessions. Meta.Window ids are decimal strings; the shell's own
 * `global.get_window_actors()` is the only place they come from, which is why
 * the id has to be resolved inside the same script.
 *
 * Matching is by WM_CLASS or title substring, because nothing else survives
 * the trip: AT-SPI knows the app's name and title, not Meta's window id.
 */
char *cu_gnome_activate_by_title(const char *title_hint) {
  if (!title_hint || !*title_hint) return NULL;
  if (cu_gnome_eval_state() != CU_GNOME_EVAL_OK) return NULL;

  /* JSON-escape the hint so a quote in a window title cannot break out of the
     JS string literal we are building. */
  GString *escaped = g_string_new(NULL);
  for (const char *p = title_hint; *p; p++) {
    switch (*p) {
      case '"':  g_string_append(escaped, "\\\""); break;
      case '\\': g_string_append(escaped, "\\\\"); break;
      case '\n': g_string_append(escaped, "\\n"); break;
      case '\r': g_string_append(escaped, "\\r"); break;
      default:   g_string_append_c(escaped, *p); break;
    }
  }

  char *script = g_strdup_printf(
      "(function(){"
      "  var hint = \"%s\";"
      "  var actors = global.get_window_actors();"
      "  for (var i = 0; i < actors.length; i++) {"
      "    var w = actors[i].meta_window;"
      "    if (!w) continue;"
      "    var t = w.get_title() || \"\";"
      "    if (t === hint || t.indexOf(hint) >= 0) {"
      "      w.activate(global.get_current_time());"
      "      return \"activated\";"
      "    }"
      "  }"
      "  return \"not-found\";"
      "})()",
      escaped->str);
  g_string_free(escaped, TRUE);

  char *result = cu_gnome_eval(script);
  g_free(script);
  if (!result) return NULL;
  if (strcmp(result, "activated") == 0) {
    g_free(result);
    return g_strdup("Window activated through GNOME Shell.");
  }
  g_free(result);
  return NULL;
}

/* ----------------------- Silent screenshot via Shell ---------------------- */

/*
 * Cheap gate before attempting a capture: is the GNOME Shell screenshot service
 * on the bus at all? Called on every get_window_state, so it is worth a direct
 * NameHasOwner rather than a full desktop re-detection (which would also poke
 * gsettings and the portal versions).
 */
bool cu_gnome_screenshot_path_available(void) {
  return cu_bus_name_has_owner("org.gnome.Shell.Screenshot");
}

/*
 * org.gnome.Shell.Screenshot.Screenshot(include_cursor, flash, filename)
 *   -> (success, filename_used)
 *
 * `filename` may be an absolute path, which is what we want: we hand it a
 * private temporary path so the capture does not litter the user's Pictures
 * folder the way the portal does. flash=false keeps it silent; include_cursor
 * follows the MCP layer's convention that the model may need to see the pointer
 * it just moved, so we ask for it.
 *
 * ScreenshotArea(x, y, width, height, flash, filename) does the cropping in the
 * compositor, which is strictly better than capturing everything and cropping
 * in-process: no full-screen NPOT texture, and no wasted base64.
 */
char *cu_gnome_screenshot_to_file(int x, int y, int w, int h, bool include_cursor) {
  if (!gnome_shell_present()) return NULL;

  char *tmpl = g_build_filename(g_get_tmp_dir(), "lobster-cu-XXXXXX.png", NULL);
  int fd = g_mkstemp_full(tmpl, G_FILE_TEST_IS_REGULAR, 0600);
  if (fd < 0) {
    g_free(tmpl);
    return NULL;
  }
  close(fd);
  /* g_mkstemp replaced the XXXXXX with a unique suffix but kept no .png;
     gnome-shell writes PNG regardless of extension, and cairo sniffs content,
     so the name only matters for our own bookkeeping. */

  GError *err = NULL;
  GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!conn) {
    if (err) g_error_free(err);
    unlink(tmpl);
    g_free(tmpl);
    return NULL;
  }

  GVariant *reply = NULL;
  if (w > 0 && h > 0) {
    reply = g_dbus_connection_call_sync(
        conn, "org.gnome.Shell.Screenshot", "/org/gnome/Shell/Screenshot",
        "org.gnome.Shell.Screenshot", "ScreenshotArea",
        g_variant_new("(iiiibs)", x, y, w, h, FALSE, tmpl),
        G_VARIANT_TYPE("(bs)"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &err);
  } else {
    reply = g_dbus_connection_call_sync(
        conn, "org.gnome.Shell.Screenshot", "/org/gnome/Shell/Screenshot",
        "org.gnome.Shell.Screenshot", "Screenshot",
        g_variant_new("(bbs)", include_cursor ? TRUE : FALSE, FALSE, tmpl),
        G_VARIANT_TYPE("(bs)"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &err);
  }
  g_object_unref(conn);

  if (!reply) {
    if (err) {
      g_debug("[gnome] screenshot failed: %s", err->message);
      g_error_free(err);
    }
    unlink(tmpl);
    g_free(tmpl);
    return NULL;
  }

  gboolean ok = FALSE;
  const char *used = NULL;
  g_variant_get(reply, "(b&s)", &ok, &used);
  char *path = (ok && used && *used) ? g_strdup(used) : NULL;
  g_variant_unref(reply);

  unlink(tmpl);
  g_free(tmpl);
  return path;
}

/* ---------------------------- Accessibility switch ------------------------ */

/*
 * GNOME's accessibility opt-in. Reading goes through the portal (so it works
 * without linking dconf); writing goes through gsettings, because the portal is
 * read-only and there is no other supported way to write a setting.
 *
 * Turning this on does not retrofit already-running applications: GTK reads the
 * setting when the toolkit initialises, so the user must restart an app for it
 * to appear in the tree. That is why the message says so instead of claiming
 * the problem is fixed.
 */
bool cu_gnome_set_toolkit_accessibility(bool enabled) {
  const char *value = enabled ? "true" : "false";
  char *argv[] = {
    (char *)"gsettings",
    (char *)"set",
    (char *)"org.gnome.desktop.interface",
    (char *)"toolkit-accessibility",
    (char *)value,
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
    return false;
  }
  return g_spawn_check_wait_status(status, NULL);
}

/* --------------------------- GNOME version probe -------------------------- */

/* org.gnome.Shell.ShellVersion is a property on the private interface; it is
   read-only and unprivileged, so this is a cheap way to tailor advice. */
char *cu_gnome_shell_version(void) {
  if (!gnome_shell_present()) return NULL;
  GError *err = NULL;
  GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!conn) {
    if (err) g_error_free(err);
    return NULL;
  }
  GVariant *reply = g_dbus_connection_call_sync(
      conn, "org.gnome.Shell", "/org/gnome/Shell",
      "org.freedesktop.DBus.Properties", "Get",
      g_variant_new("(ss)", "org.gnome.Shell", "ShellVersion"),
      G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err);
  g_object_unref(conn);
  if (!reply) {
    if (err) g_error_free(err);
    return NULL;
  }
  /* "(v)" already unwrapped the outer variant (see cu_portal_version). */
  GVariant *v = NULL;
  g_variant_get(reply, "(v)", &v);
  char *version = NULL;
  if (v) {
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING))
      version = g_variant_dup_string(v, NULL);
    g_variant_unref(v);
  }
  g_variant_unref(reply);
  return version;
}

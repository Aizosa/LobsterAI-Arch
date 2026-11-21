/*
 * Desktop environment detection and the per-desktop capability matrix.
 *
 * Linux is not one platform. The same helper binary runs under KDE Plasma,
 * GNOME, XFCE, MATE, Cinnamon, Sway, Hyprland and a dozen others, on two
 * session protocols, and what works differs on every axis. Rather than
 * scattering `getenv("XDG_CURRENT_DESKTOP")` checks through the backends, every
 * decision goes through one place: the CuDesktop struct below.
 *
 * What actually varies, and why:
 *
 *   - Capture. On X11 we can usually read a window's pixels. On Wayland we
 *     cannot read anything ourselves (the protocol is explicit about this),
 *     so we tunnel through xdg-desktop-portal, whose backend is provided by
 *     the desktop itself. GNOME's backend is xdg-desktop-portal-gnome, KDE's
 *     is -kde, XFCE historically had none and fell back to -gtk. GNOME and
 *     KDE additionally expose their own private D-Bus screenshot APIs, which
 *     are silent and prompt-free where the portal may not be.
 *
 *   - Input injection. X11 has XTest, which needs no permission at all.
 *     Wayland deliberately forbids a client from synthesizing input for
 *     another client; the sanctioned path is libei through the RemoteDesktop
 *     portal, which pops a consent dialog. Some compositors expose suite-
 *     specific escape hatches (GNOME Shell's private Eval on X11 only).
 *
 *   - Accessibility. AT-SPI2 is the same everywhere, but whether applications
 *     are *in* the tree depends on a per-desktop setting that defaults off on
 *     GNOME and varies elsewhere, and on the toolkit honouring it. That switch
 *     is worth detecting because its absence produces an empty window list,
 *     which is otherwise a baffling failure.
 *
 *   - Window management. X11 has EWMH; every compliant WM implements
 *     _NET_ACTIVE_WINDOW and _NET_CLOSE_WINDOW. Wayland has no client-facing
 *     equivalent by design, and the compositors differ in how much they will
 *     bend (KWin forwards AT-SPI grab_focus, GNOME does not).
 */
#include "cu.h"

#include <gio/gio.h>
#include <string.h>

/* ------------------------------- detection ------------------------------- */

/* XDG_CURRENT_DESKTOP is a colon-separated list, most specific first:
   "KDE", "GNOME", "ubuntu:GNOME", "XFCE", "X-Cinnamon", "sway", "Hyprland".
   We match on substrings so the Ubuntu flavours work. */
static bool desktop_list_has(const char *list, const char *needle) {
  if (!list) return false;
  char **parts = g_strsplit(list, ":", -1);
  bool found = false;
  for (int i = 0; parts[i]; i++) {
    if (g_ascii_strcasecmp(parts[i], needle) == 0) {
      found = true;
      break;
    }
  }
  g_strfreev(parts);
  return found;
}

const char *cu_session_type(void) {
  const char *t = g_getenv("XDG_SESSION_TYPE");
  if (t && *t) return t;
  /* No XDG_SESSION_TYPE at all (systemd-less sessions, or a helper launched
     from a container): infer from whichever socket is advertised. */
  if (g_getenv("WAYLAND_DISPLAY")) return "wayland";
  if (g_getenv("DISPLAY")) return "x11";
  if (g_getenv("XDG_CURRENT_DESKTOP") || g_getenv("XDG_SESSION_DESKTOP"))
    return "wayland";   /* modern default; corrected by capability probes */
  return "unknown";
}

bool cu_session_is_wayland(void) {
  return strcmp(cu_session_type(), "wayland") == 0;
}

/* Best-effort human name for the running desktop, used in messages. */
static const char *desktop_display_name(CuDesktopKind kind) {
  switch (kind) {
    case CU_DE_KDE:      return "KDE Plasma";
    case CU_DE_GNOME:    return "GNOME";
    case CU_DE_XFCE:     return "XFCE";
    case CU_DE_MATE:     return "MATE";
    case CU_DE_CINNAMON: return "Cinnamon";
    case CU_DE_LXQT:     return "LXQt";
    case CU_DE_WLROOTS:  return "a wlroots compositor";
    default:             return "an unrecognised desktop";
  }
}

/* Does a name appear in $PATH? Used to report which fallbacks are usable. */
bool cu_have_command(const char *name) {
  if (!name) return false;
  const char *path = g_getenv("PATH");
  if (!path) return false;
  char **dirs = g_strsplit(path, ":", -1);
  bool found = false;
  for (int i = 0; dirs[i] && !found; i++) {
    if (!*dirs[i]) continue;
    char *full = g_build_filename(dirs[i], name, NULL);
    found = g_file_test(full, G_FILE_TEST_IS_EXECUTABLE);
    g_free(full);
  }
  g_strfreev(dirs);
  return found;
}

/* Session-bus singleton check. Same trick D-Bus itself uses: a well-known
   name can only be owned by one connection, so a failed request means the
   name is not currently on the bus. Note this is *not* sufficient to prove the
   interface exists — GNOME Shell restricts Eval to X11 sessions while the name
   is still owned — so callers must also be ready for UnknownMethod. */
bool cu_bus_name_has_owner(const char *name) {
  if (!name) return false;
  GError *err = NULL;
  GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!conn) {
    if (err) g_error_free(err);
    return false;
  }
  GVariant *reply = g_dbus_connection_call_sync(
      conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", name),
      G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err);
  g_object_unref(conn);
  if (!reply) {
    if (err) g_error_free(err);
    return false;
  }
  gboolean owned = FALSE;
  g_variant_get(reply, "(b)", &owned);
  g_variant_unref(reply);
  return owned;
}

void cu_desktop_detect(CuDesktop *out) {
  memset(out, 0, sizeof(*out));

  const char *current = g_getenv("XDG_CURRENT_DESKTOP");
  const char *session_desktop = g_getenv("XDG_SESSION_DESKTOP");
  const char *session = cu_session_type();

  out->kind = CU_DE_UNKNOWN;
  if (desktop_list_has(current, "KDE") || desktop_list_has(session_desktop, "KDE") ||
      g_getenv("KDE_FULL_SESSION")) {
    out->kind = CU_DE_KDE;
  } else if (desktop_list_has(current, "GNOME") ||
             desktop_list_has(current, "ubuntu") ||
             desktop_list_has(session_desktop, "gnome")) {
    out->kind = CU_DE_GNOME;
  } else if (desktop_list_has(current, "XFCE") ||
             desktop_list_has(session_desktop, "xfce")) {
    out->kind = CU_DE_XFCE;
  } else if (desktop_list_has(current, "MATE") ||
             desktop_list_has(session_desktop, "mate")) {
    out->kind = CU_DE_MATE;
  } else if (desktop_list_has(current, "X-Cinnamon") ||
             desktop_list_has(current, "Cinnamon")) {
    out->kind = CU_DE_CINNAMON;
  } else if (desktop_list_has(current, "LXQt")) {
    out->kind = CU_DE_LXQT;
  } else if (desktop_list_has(current, "sway") ||
             desktop_list_has(current, "Hyprland") ||
             desktop_list_has(current, "river") ||
             desktop_list_has(current, "Wayfire")) {
    out->kind = CU_DE_WLROOTS;
  }

  out->name = desktop_display_name(out->kind);
  out->session_type = session;
  out->is_wayland = strcmp(session, "wayland") == 0;
  out->is_x11 = strcmp(session, "x11") == 0;

  /* X11 is available whenever DISPLAY is set — including under XWayland, where
     X clients still exist and XTest still works on them. That is why an XWayland
     session reports both is_wayland and has_x11. */
  const char *display = g_getenv("DISPLAY");
  out->has_x11 = display != NULL && *display != '\0';

  /* The portal is the only portable capture path on Wayland, and a useful
     silent one on X11 too. Detect it by asking for the interface version,
     which fails cleanly when no portal is installed. */
  out->has_portal = false;
  out->portal_screenshot_version = 0;
  out->portal_screencast_version = 0;
  out->portal_remote_desktop_version = 0;
  out->portal_settings_version = 0;
  if (cu_bus_name_has_owner("org.freedesktop.portal.Desktop")) {
    out->has_portal = true;
    out->portal_screenshot_version = cu_portal_version("org.freedesktop.portal.Screenshot");
    out->portal_screencast_version = cu_portal_version("org.freedesktop.portal.ScreenCast");
    out->portal_remote_desktop_version = cu_portal_version("org.freedesktop.portal.RemoteDesktop");
    out->portal_settings_version = cu_portal_version("org.freedesktop.portal.Settings");
  }

  /* Suite-specific private APIs. Their presence decides whether we can capture
     without asking the portal at all. */
  out->gnome_shell = cu_bus_name_has_owner("org.gnome.Shell");
  out->gnome_shell_screenshot =
      out->gnome_shell && cu_bus_name_has_owner("org.gnome.Shell.Screenshot");

  /* GNOME's accessibility opt-in. Absent gsettings or schema, assume true:
     an unknown state should not be reported as "broken". */
  out->toolkit_accessibility = cu_gsettings_bool(
      "org.gnome.desktop.interface", "toolkit-accessibility", true);

  /* Command-line capture fallbacks, most useful on desktops whose portal
     backend does not implement Screenshot (XFCE with -gtk, older versions). */
  out->cmd_spectacle = cu_have_command("spectacle");
  out->cmd_grim = cu_have_command("grim");
  out->cmd_gnome_screenshot = cu_have_command("gnome-screenshot");
  out->cmd_xfce4_screenshooter = cu_have_command("xfce4-screenshooter");

  /* X11 window manager identity and EWMH support. A bare X server (Xvfb, or a
     session whose WM died) has no _NET_SUPPORTING_WM_CHECK and no client list,
     and EWMH requests silently do nothing there — worth knowing before we
     promise a window can be raised. */
  out->x11_wm_name = NULL;
  out->x11_ewmh = false;
  out->x11_has_active_window = false;
  out->x11_has_close_window = false;
  if (out->has_x11) cu_x11_detect_wm(out);
}

/* --------------------------- GSettings access ---------------------------- */

/*
 * We read GSettings through the portal rather than by linking libgio's
 * GSettings — same data, but it works when the helper runs outside the user's
 * session D-Bus in a way that breaks dconf client setup, and it avoids pulling
 * in a dconf dependency for one boolean.
 *
 * org.freedesktop.portal.Settings.ReadOne exists since portal version 1 but
 * returned a variant nested in a variant in some early implementations; both
 * shapes are accepted below.
 */
bool cu_gsettings_bool(const char *schema, const char *key, bool fallback) {
  GError *err = NULL;
  GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!conn) {
    if (err) g_error_free(err);
    return fallback;
  }

  GVariant *reply = g_dbus_connection_call_sync(
      conn, "org.freedesktop.portal.Desktop",
      "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.Settings", "ReadOne",
      g_variant_new("(ss)", schema, key),
      G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err);
  g_object_unref(conn);

  if (!reply) {
    if (err) g_error_free(err);
    return fallback;
  }

  /* "(v)" unwraps the outer variant for us: `value` is already the setting's
     own type (a boolean for toolkit-accessibility), not a variant around it. */
  GVariant *value = NULL;
  g_variant_get(reply, "(v)", &value);
  bool result = fallback;
  if (value) {
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN))
      result = g_variant_get_boolean(value) ? true : false;
    else if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING))
      result = g_ascii_strcasecmp(g_variant_get_string(value, NULL), "true") == 0;
    g_variant_unref(value);
  }
  g_variant_unref(reply);
  return result;
}

/* --------------------------------- report -------------------------------- */

char *cu_desktop_report_json(void) {
  CuDesktop de;
  cu_desktop_detect(&de);

  CuJson j; cu_json_init(&j);
  cu_json_obj_begin(&j);

  cu_json_key(&j, "desktop"); cu_json_str(&j, de.name);
  cu_json_key(&j, "desktopId"); cu_json_str(&j, cu_desktop_kind_id(de.kind));
  cu_json_key(&j, "sessionType"); cu_json_str(&j, de.session_type);
  cu_json_key(&j, "xwayland"); cu_json_bool(&j, de.is_wayland && de.has_x11);

  cu_json_key_obj(&j, "portal");
  cu_json_key(&j, "available"); cu_json_bool(&j, de.has_portal);
  cu_json_key(&j, "screenshotVersion"); cu_json_int(&j, de.portal_screenshot_version);
  cu_json_key(&j, "screencastVersion"); cu_json_int(&j, de.portal_screencast_version);
  cu_json_key(&j, "remoteDesktopVersion"); cu_json_int(&j, de.portal_remote_desktop_version);
  cu_json_key(&j, "settingsVersion"); cu_json_int(&j, de.portal_settings_version);
  cu_json_obj_end(&j);

  cu_json_key_obj(&j, "suiteApis");
  cu_json_key(&j, "gnomeShell"); cu_json_bool(&j, de.gnome_shell);
  cu_json_key(&j, "gnomeShellScreenshot"); cu_json_bool(&j, de.gnome_shell_screenshot);
  cu_json_key_obj(&j, "gnomeShellEval");
  /* Eval is reachable whenever the shell is; it only *works* on X11, which is
     a property of the call rather than of the interface's presence. */
  cu_json_key(&j, "exposed"); cu_json_bool(&j, de.gnome_shell);
  cu_json_key(&j, "functional"); cu_json_bool(&j, de.gnome_shell && !de.is_wayland);
  cu_json_obj_end(&j);
  cu_json_obj_end(&j);

  cu_json_key_obj(&j, "capture");
  cu_json_key(&j, "preferred"); cu_json_str(&j, cu_capture_preferred_backend());
  cu_json_key(&j, "available");
  cu_json_obj_begin(&j);
  cu_json_key(&j, "x11WindowPixels"); cu_json_bool(&j, de.has_x11);
  cu_json_key(&j, "portalScreenshot"); cu_json_bool(&j, de.portal_screenshot_version > 0);
  cu_json_key(&j, "gnomeShell"); cu_json_bool(&j, de.gnome_shell_screenshot);
  cu_json_key(&j, "spectacle"); cu_json_bool(&j, de.cmd_spectacle);
  cu_json_key(&j, "grim"); cu_json_bool(&j, de.cmd_grim);
  cu_json_key(&j, "gnomeScreenshot"); cu_json_bool(&j, de.cmd_gnome_screenshot);
  cu_json_key(&j, "xfce4Screenshooter"); cu_json_bool(&j, de.cmd_xfce4_screenshooter);
  cu_json_obj_end(&j);
  cu_json_obj_end(&j);

  cu_json_key_obj(&j, "input");
  cu_json_key(&j, "preferred"); cu_json_str(&j, cu_input_preferred_backend());
  cu_json_key(&j, "xtest"); cu_json_bool(&j, de.has_x11);
  cu_json_key(&j, "remoteDesktopPortal"); cu_json_bool(&j, de.portal_remote_desktop_version > 0);
  cu_json_key(&j, "remoteDesktopSession"); cu_json_bool(&j, cu_remote_desktop_active());
  cu_json_obj_end(&j);

  cu_json_key_obj(&j, "accessibility");
  /*
   * Two different questions, and conflating them is how this misled us once
   * already: the bus can be running (GetAddress answers) while this process
   * still failed to attach to it (atspi_init returned non-zero). "bus" is
   * liveness, "attached" is whether windows and elements will actually be
   * reachable from here. Only "attached" predicts what the next tool call does.
   */
  cu_json_key(&j, "bus"); cu_json_bool(&j, cu_atspi_bus_reachable());
  cu_json_key(&j, "attached"); cu_json_bool(&j, cu_atspi_attached());
  cu_json_key(&j, "toolkitAccessibility"); cu_json_bool(&j, de.toolkit_accessibility);
  cu_json_obj_end(&j);

  cu_json_key_obj(&j, "windowManager");
  if (de.has_x11) {
    cu_json_key(&j, "x11Wm"); cu_json_str(&j, de.x11_wm_name ? de.x11_wm_name : "none");
    cu_json_key(&j, "ewmh"); cu_json_bool(&j, de.x11_ewmh);
    cu_json_key(&j, "closeWindow"); cu_json_bool(&j, de.x11_has_close_window);
    cu_json_key(&j, "activeWindow"); cu_json_bool(&j, de.x11_has_active_window);
  } else {
    cu_json_key(&j, "x11Wm"); cu_json_null(&j);
    cu_json_key(&j, "ewmh"); cu_json_bool(&j, false);
    cu_json_key(&j, "closeWindow"); cu_json_bool(&j, false);
    cu_json_key(&j, "activeWindow"); cu_json_bool(&j, false);
  }
  cu_json_obj_end(&j);

  cu_json_obj_end(&j);

  char *out = g_strdup(j.buf->str);
  cu_json_free(&j);
  g_free(de.x11_wm_name);
  return out;
}

const char *cu_desktop_kind_id(CuDesktopKind kind) {
  switch (kind) {
    case CU_DE_KDE:      return "kde";
    case CU_DE_GNOME:    return "gnome";
    case CU_DE_XFCE:     return "xfce";
    case CU_DE_MATE:     return "mate";
    case CU_DE_CINNAMON: return "cinnamon";
    case CU_DE_LXQT:     return "lxqt";
    case CU_DE_WLROOTS:  return "wlroots";
    default:             return "unknown";
  }
}

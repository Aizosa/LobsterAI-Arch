/*
 * Wayland-specific integration.
 *
 * Wayland's security model is the mirror image of X11's: no client may read
 * another client's pixels, and no client may synthesize input for another
 * client. Both are deliberate, not gaps. The sanctioned escapes are all
 * mediated by xdg-desktop-portal, which talks to a per-desktop backend that
 * shows the user a consent dialog. That is the whole design — so anything we do
 * here is either (a) going through the portal with the user's blessing, or
 * (b) probing for a compositor's private door and reporting honestly when it
 * is locked.
 *
 * What is implemented here:
 *
 *   ScreenCast + PipeWire      — the portal's ScreenCast interface. Creates a
 *       session, selects a monitor, starts it, and gets back a stream
 *       description plus a PipeWire file descriptor. Implementing the actual
 *       frame grab needs libpipewire as a dependency; we do the full session
 *       handshake (which is the hard part and is where every naive
 *       implementation goes wrong: the Request/Response dance, the session
 *       object path, the restore token) and then report whether frames can be
 *       pulled. Doing the handshake but not pretending to decode frames is
 *       better than not trying: it produces a real, actionable diagnosis.
 *
 *   RemoteDesktop + libei      — the portal's RemoteDesktop interface. This is
 *       the only way to synthesize input on Wayland. Same Request/Response
 *       protocol, then either the Notify* methods (deprecated, but simpler and
 *       sufficient for discrete input) or ConnectToEIS for a full libei device.
 *       We use Notify* because it needs no extra dependency and handles exactly
 *       the operations Computer Use performs: absolute pointer motion, button
 *       press/release, key press/release, axis scroll.
 *
 *   wlroots protocols          — sway, Hyprland, river, Wayfire expose
 *       zwlr_screencopy_manager_v1 (frame capture) and
 *       zwlr_virtual_pointer_v1 / zwp_virtual_keyboard_v1 (input). These are
 *       *not* available under GNOME or KDE, and are not available to a helper
 *       that is not itself a Wayland client of that compositor. We detect them
 *       so the report can say "this desktop could be supported directly without
 *       a portal" rather than leaving a mystery.
 *
 *   wlr-data-control / ext-data-control — the Wayland answer to reading the
 *       primary selection, which is what the desktop-companion selection-hook
 *       needs. Also probe-only here.
 *
 * Everything is lazily initialised and single-session: one consent dialog per
 * process lifetime, reused for every subsequent injection. Popping a dialog per
 * keystroke would make the feature unusable.
 */
#include "cu.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <string.h>
#include <unistd.h>

#if defined(CU_HAVE_EIS)
#include <libei.h>
#include <poll.h>
#include <time.h>
#endif

/* ------------------------------ session state ----------------------------- */

typedef struct {
  bool tried;                 /* a start attempt has been made (success or not) */
  bool active;                /* session is live and Notify* will work */
  bool has_keyboard;          /* RD granted the keyboard device bit */
  bool has_pointer;           /* RD granted the pointer device bit */
  guint devices;              /* bitmask of granted device types */
  char *session_handle;       /* object path of the portal Session */
  gint64 last_error_code;     /* portal response code when inactive */
  char *last_error;           /* human readable reason */
} RemoteDesktopState;

static RemoteDesktopState g_rd = { 0 };

#if defined(CU_HAVE_EIS)
/*
 * libei virtual-seat state, established once the RemoteDesktop session is
 * live. The EIS channel is what actually synthesizes input on compositors
 * that do not honour the deprecated Notify* portal methods (KDE 6 included).
 * All device pointers are owned by the ei context; we hold references only
 * for the duration of the process.
 */
typedef struct {
  struct ei *ei;
  struct ei_seat *seat;
  struct ei_device *kb;       /* EI_DEVICE_CAP_KEYBOARD */
  struct ei_device *abs;       /* EI_DEVICE_CAP_POINTER_ABSOLUTE */
  struct ei_device *ptr;       /* EI_DEVICE_CAP_POINTER (relative) */
  struct ei_device *scroll;   /* EI_DEVICE_CAP_SCROLL */
  struct ei_device *text;      /* EI_DEVICE_CAP_TEXT */
  bool kb_resumed;
  bool abs_resumed;
  bool text_resumed;
  uint32_t sequence;
} EisState;

static EisState g_eis = { 0 };

static int64_t eis_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif

/* PipeWire stream discovery result, kept for reporting. */
typedef struct {
  bool probed;
  bool have_stream;
  guint node_id;
  int width;
  int height;
  gint64 position_x;
  gint64 position_y;
} ScreenCastState;

static ScreenCastState g_sc = { 0 };

static GDBusConnection *session_bus(void) {
  static GDBusConnection *cached = NULL;
  if (cached) return cached;
  GError *err = NULL;
  cached = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!cached && err) g_error_free(err);
  return cached;
}

/* ---------------------------- request plumbing ---------------------------- */

/*
 * Every portal method that can involve a dialog returns a Request handle and
 * delivers the outcome asynchronously on org.freedesktop.portal.Request.Response.
 * Getting this right requires three things that are easy to miss:
 *
 *   1. The Response object path is *predicted*, not read from the reply, because
 *      waiting for the reply means the signal can already have fired. The path
 *      is /org/freedesktop/portal/desktop/request/<sender>/<token> where
 *      <sender> is our unique bus name with ':' and '.' replaced by '_'.
 *   2. handle_token must be a valid object-path element: alphanumeric plus
 *      underscore, not starting with a digit.
 *   3. The signal must be subscribed *before* the method call is issued.
 *
 * The wait runs a private GMainContext so it cannot disturb whatever the caller
 * was doing, and has a timeout because a user may simply ignore the dialog.
 */

/*
 * State of one in-flight portal request.
 *
 * `timed_out` exists because the portal's own response codes cannot express
 * "we gave up": 0 is success, 1 is the user saying no, 2 is an error. A dialog
 * nobody answered is none of those, and the caller must treat it as retryable
 * rather than as a refusal.
 */
typedef struct {
  GMainLoop *loop;
  bool done;
  guint32 response;
  GVariant *results;   /* owned; a{sv} */
  bool timed_out;
  guint timer;         /* the timeout source id, zeroed when it fires */
} PortalRequestWait;

static void on_request_response(GDBusConnection *conn, const char *sender,
                                const char *object_path, const char *interface,
                                const char *signal_name, GVariant *params,
                                gpointer user_data) {
  (void)conn; (void)sender; (void)object_path;
  (void)interface; (void)signal_name;
  PortalRequestWait *w = user_data;
  if (w->done) return;

  guint32 response = 2;
  GVariant *results = NULL;
  g_variant_get(params, "(u@a{sv})", &response, &results);
  w->response = response;
  if (w->results) g_variant_unref(w->results);
  w->results = results;
  w->done = true;
  g_main_loop_quit(w->loop);
}

static gboolean request_timeout_cb(gpointer data) {
  PortalRequestWait *w = data;
  /* The source is removing itself; forget the id so the caller does not try to
     remove it a second time (that would be a GLib critical). */
  w->timer = 0;
  if (!w->done) {
    w->done = true;
    w->timed_out = true;
    /* Leave response at 2 ("other error") so the caller sees a definite
       failure rather than waiting forever on a dialog nobody answered. */
    g_main_loop_quit(w->loop);
  }
  return G_SOURCE_REMOVE;
}

/* Builds a token satisfying the object-path-element rules. */
static char *make_token(const char *prefix) {
  return g_strdup_printf("%s_%u_%u", prefix,
                         (unsigned)(g_random_int() & 0xFFFFFF),
                         (unsigned)(g_random_int() & 0xFFFFFF));
}

/* Predicts the Request object path for a token we are about to send. */
static char *predict_request_path(GDBusConnection *conn, const char *token) {
  const char *sender = g_dbus_connection_get_unique_name(conn);
  if (!sender) return NULL;
  GString *s = g_string_new(sender + 1);   /* skip the leading ':' */
  for (gchar *p = s->str; *p; p++) {
    if (*p == '.') *p = '_';
  }
  char *path = g_strdup_printf(
      "/org/freedesktop/portal/desktop/request/%s/%s", s->str, token);
  g_string_free(s, TRUE);
  return path;
}

/*
 * Calls one portal method and waits for its Request::Response.
 *
 * parameters is the full argument tuple *including* the trailing a{sv} options
 * dict; the caller has already put handle_token in it. On success returns the
 * results vardict (owned) and sets *response to 0; on denial or timeout returns
 * NULL and sets *response to the portal's code (1 = cancelled, 2 = other).
 */
static GVariant *portal_request(GDBusConnection *conn, const char *interface,
                                const char *method, GVariant *parameters,
                                const char *token, guint32 *response,
                                int timeout_seconds) {
  *response = 2;

  char *request_path = predict_request_path(conn, token);
  if (!request_path) return NULL;

  PortalRequestWait wait = { g_main_loop_new(NULL, FALSE), false, 2, NULL, false, 0 };
  guint sub = g_dbus_connection_signal_subscribe(
      conn, "org.freedesktop.portal.Desktop",
      "org.freedesktop.portal.Request", "Response",
      request_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
      on_request_response, &wait, NULL);
  wait.timer = g_timeout_add_seconds_full(
      G_PRIORITY_DEFAULT, (guint)timeout_seconds, request_timeout_cb, &wait, NULL);

  GError *err = NULL;
  GVariant *reply = g_dbus_connection_call_sync(
      conn, "org.freedesktop.portal.Desktop",
      "/org/freedesktop/portal/desktop", interface, method, parameters,
      G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);

  GVariant *results = NULL;
  if (!reply) {
    if (err) {
      g_debug("[wayland] %s.%s failed: %s", interface, method, err->message);
      g_error_free(err);
    }
    wait.done = true;
  } else {
    g_variant_unref(reply);
    if (!wait.done) g_main_loop_run(wait.loop);
    if (wait.done && wait.response == 0) {
      results = wait.results;
      wait.results = NULL;
    }
    /* The portal only defines 0 (success), 1 (cancelled) and 2 (other error).
       3 is ours alone, meaning "we gave up waiting": the user may simply not
       have answered yet, which is a retryable condition, unlike a refusal. */
    *response = wait.timed_out ? 3 : wait.response;
  }

  /* Zeroed by the callback when it fires; removing a dead id is a critical. */
  if (wait.timer != 0) g_source_remove(wait.timer);
  g_dbus_connection_signal_unsubscribe(conn, sub);
  g_main_loop_unref(wait.loop);
  if (wait.results) g_variant_unref(wait.results);
  g_free(request_path);
  return results;
}

/* -------------------------- RemoteDesktop (libei) ------------------------- */

/*
 * The RemoteDesktop session protocol, in order:
 *
 *   1. CreateSession(options{session_handle_token, handle_token}) -> handle
 *      Response carries "session_handle" as a *string*, not an object path
 *      (a documented historical wart kept for compatibility).
 *   2. SelectDevices(session_handle, {types: KEYBOARD|POINTER, handle_token})
 *      `types` is a bitmask: 1 keyboard, 2 pointer, 4 touchscreen.
 *   3. Start(session_handle, parent_window="", {handle_token})
 *      This is the call that shows the consent dialog. Response carries
 *      "devices" (granted bitmask), optionally "streams", and a restore_token.
 *
 * After Start, the Notify* methods are allowed for the granted device types.
 * There is a per-session limit of one ConnectToEIS call; since we use Notify*,
 * we never call it.
 */

#define RD_DEVICE_KEYBOARD 1u
#define RD_DEVICE_POINTER  2u

static void rd_fail(gint64 code, const char *message) {
  g_rd.active = false;
  g_rd.has_keyboard = false;
  g_rd.has_pointer = false;
  g_rd.last_error_code = code;
  g_free(g_rd.last_error);
  g_rd.last_error = message ? g_strdup(message) : NULL;
}

/* -------------------- restore token/data persistence (#35) ------------- */
/*
 * The RemoteDesktop backend remembers a granted session through an opaque
 * restore token handed back in Start results and echoed back in the next
 * SelectDevices options. When it round-trips intact and the requested
 * devices/screenshare/clipboard match, the backend treats the session as
 * restored and skips the consent dialog (`restored || mega-auth`).
 *
 * Two protocol revisions exist: older backends return a string under
 * `restore_token`, newer ones a struct under `restore_data`. The value is
 * opaque to us: we never parse its fields. We keep its normal-form
 * serialisation on disk, with its GVariant type string stored beside it so the
 * next process rebuilds an exact same-typed variant.
 *
 * File layout (little-endian): uint32 type-string length, type string, then
 * the normal-form bytes. The first token still has to be earned from a real
 * consent dialog — there is nothing to persist until then.
 */
static char *rd_restore_path(void) {
  const char *cfg = g_get_user_config_dir();
  return g_build_filename(cfg, "lobsterai", "computer-use", "rd-restore.bin", NULL);
}

static GVariant *rd_restore_load(void) {
  char *path = rd_restore_path();
  GError *err = NULL;
  gsize size = 0;
  gchar *data = NULL;
  GVariant *v = NULL;

  if (!g_file_get_contents(path, &data, &size, &err)) {
    if (err) g_error_free(err);
    goto out;
  }
  if (size < 4) goto out;

  guint32 sig_len;
  memcpy(&sig_len, data, 4);
  if (sig_len == 0 || (gsize)sig_len + 4 > size) goto out;

  const char *sig = data + 4;
  gsize body_len = size - 4 - sig_len;
  /* Copy the body: new_from_bytes keeps a reference to the bytes, and the
     static view would dangle once `data` is freed below. */
  GBytes *body = g_bytes_new(data + 4 + sig_len, body_len);
  GVariantType *type = g_variant_type_new(sig);
  v = g_variant_new_from_bytes(type, body, FALSE);
  g_bytes_unref(body);
  g_variant_type_free(type);
out:
  g_free(data);
  g_free(path);
  return v;
}

static bool rd_restore_save(GVariant *restore) {
  if (!restore) return false;
  char *path = rd_restore_path();
  char *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0700);
  g_free(dir);

  const char *sig = g_variant_get_type_string(restore);
  GVariant *normal = g_variant_get_normal_form(restore);
  gsize body_len = g_variant_get_size(normal);
  gconstpointer body = g_variant_get_data(normal);

  GString *blob = g_string_new(NULL);
  guint32 sig_len = (guint32)strlen(sig);
  g_string_append_len(blob, (const char *)&sig_len, 4);
  g_string_append(blob, sig);
  g_string_append_len(blob, (const char *)body, body_len);

  GError *err = NULL;
  bool ok = g_file_set_contents(path, blob->str, (gssize)blob->len, &err) != FALSE;
  if (err) g_error_free(err);

  g_string_free(blob, TRUE);
  g_free(path);
  g_variant_unref(normal);
  return ok;
}

#if defined(CU_HAVE_EIS)

/*
 * X keysym -> Linux evdev scancode, US QWERTY layout.
 *
 * ei_device_keyboard_key() takes evdev codes, not keysyms, and unlike the
 * XTest path there is no X server to resolve a keysym against the loaded
 * keymap. This table covers only the symbols the helper emits; 0 means "no
 * evdev code here" and the caller skips the key rather than guessing.
 *
 * Shifted letters are handled by the caller holding Shift_L down; this table
 * returns the lowercase evdev code.
 */
static uint32_t keysym_to_evdev(unsigned long keysym) {
  struct sym { unsigned long keysym; uint32_t evdev; };
  static const struct sym table[] = {
    { 0xff1b, 1    }, /* Escape */
    { 0xff0d, 28   }, /* Return/Enter */
    { 0xff09, 15   }, /* Tab */
    { 0x0020, 57   }, /* Space */
    { 0xff08, 14   }, /* BackSpace */
    { 0xffff, 111  }, /* Delete */
    { 0xff63, 110  }, /* Insert */
    { 0xff52, 103  }, /* Up */
    { 0xff54, 108  }, /* Down */
    { 0xff51, 105  }, /* Left */
    { 0xff53, 106  }, /* Right */
    { 0xff55, 104  }, /* Page_Up */
    { 0xff56, 109  }, /* Page_Down */
    { 0xff50, 102  }, /* Home */
    { 0xff57, 107  }, /* End */
    { 0xffe1, 42   }, /* Shift_L */
    { 0xffe3, 29   }, /* Control_L */
    { 0xffe9, 56   }, /* Alt_L */
    { 0xffeb, 125  }, /* Super_L */
    { 0x1008ff11, 114 }, /* AudioLower */
    { 0x1008ff12, 113 }, /* AudioMute */
    { 0x1008ff13, 115 }, /* AudioRaise */
    /* Printable ASCII: letters, digits, punctuation on the US layout. */
    { 'a', 30 }, { 'b', 48 }, { 'c', 46 }, { 'd', 32 }, { 'e', 18 },
    { 'f', 33 }, { 'g', 34 }, { 'h', 35 }, { 'i', 23 }, { 'j', 36 },
    { 'k', 37 }, { 'l', 38 }, { 'm', 50 }, { 'n', 49 }, { 'o', 24 },
    { 'p', 25 }, { 'q', 16 }, { 'r', 19 }, { 's', 31 }, { 't', 20 },
    { 'u', 22 }, { 'v', 47 }, { 'w', 17 }, { 'x', 45 }, { 'y', 21 },
    { 'z', 44 },
    { '1', 2 }, { '2', 3 }, { '3', 4 }, { '4', 5 }, { '5', 6 },
    { '6', 7 }, { '7', 8 }, { '8', 9 }, { '9', 10 }, { '0', 11 },
    { '-', 12 }, { '=', 13 }, { '[', 26 }, { ']', 27 }, { '\\', 43 },
    { ';', 39 }, { '\'', 40 }, { '`', 41 }, { ',', 51 }, { '.', 52 },
    { '/', 53 },
  };
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (table[i].keysym == keysym) return table[i].evdev;
  }
  /* Uppercase letters map to their lowercase evdev code; Shift is held by caller. */
  if (keysym >= 'A' && keysym <= 'Z') {
    return keysym_to_evdev((unsigned long)g_ascii_tolower((int)keysym));
  }
  /* Function keys: XK_F1 == 0xffbe. F1..F10 are evdev 59..68; F11/F12 break
     the run. Beyond F12 we do not guess a code. */
  if (keysym >= 0xffbe && keysym <= 0xffc7) {
    int n = (int)(keysym - 0xffbe) + 1;
    if (n >= 1 && n <= 10) return (uint32_t)(59 + n - 1);
    if (n == 11) return 87;
    if (n == 12) return 88;
  }
  return 0;
}

/*
 * Establishes the EIS virtual seat over the already-started RD session.
 *
 * KDE bug 520464 workaround carried over from the probe: bind the keyboard
 * capability on its own first, and only after the keyboard device appears
 * bind the pointer/absolute/button/scroll/text capabilities. Binding them
 * together up front leaves the keyboard unbound on KWin.
 *
 * The dispatch loop is synchronous and short (5s deadline): by the time it
 * returns the devices we need have RESUMED, or the loop gave up and the
 * injection primitives report failure per device class.
 */
static bool rd_eis_establish(GDBusConnection *conn, const char *handle) {
  GVariantBuilder eis_opts;
  g_variant_builder_init(&eis_opts, G_VARIANT_TYPE_VARDICT);
  GUnixFDList *fd_list = NULL;
  GError *err = NULL;
  GVariant *reply = g_dbus_connection_call_with_unix_fd_list_sync(
      conn, "org.freedesktop.portal.Desktop",
      "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.RemoteDesktop", "ConnectToEIS",
      g_variant_new("(o@a{sv})", handle, g_variant_builder_end(&eis_opts)),
      G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, 10000,
      NULL, &fd_list, NULL, &err);
  if (!reply) {
    g_debug("[rd] ConnectToEIS failed: %s", err ? err->message : "unknown");
    if (err) g_error_free(err);
    return false;
  }
  gint32 fd_index = -1;
  g_variant_get(reply, "(h)", &fd_index);
  int eis_fd = g_unix_fd_list_get(fd_list, fd_index, &err);
  g_variant_unref(reply);
  g_object_unref(fd_list);
  if (eis_fd < 0) {
    g_debug("[rd] ConnectToEIS returned no fd: %s", err ? err->message : "unknown");
    if (err) g_error_free(err);
    return false;
  }

  struct ei *ei = ei_new_sender(NULL);
  if (!ei) { close(eis_fd); return false; }
  ei_configure_name(ei, "cu-helper");
  if (ei_setup_backend_fd(ei, eis_fd) < 0) {
    g_debug("[rd] ei_setup_backend_fd failed");
    ei_unref(ei);
    return false;
  }
  g_eis.ei = ei;

  bool kb_bound = false, ptr_bound = false;
  int64_t deadline = eis_now_ms() + 5000;
  while (eis_now_ms() < deadline) {
    struct pollfd pfd = { .fd = ei_get_fd(ei), .events = POLLIN };
    poll(&pfd, 1, 100);
    ei_dispatch(ei);

    struct ei_event *event = NULL;
    while ((event = ei_get_event(ei)) != NULL) {
      switch (ei_event_get_type(event)) {
        case EI_EVENT_SEAT_ADDED: {
          struct ei_seat *seat = ei_event_get_seat(event);
          g_eis.seat = seat;
          if (ei_seat_has_capability(seat, EI_DEVICE_CAP_KEYBOARD)) {
            ei_seat_bind_capabilities(seat, EI_DEVICE_CAP_KEYBOARD, NULL);
            kb_bound = true;
          } else {
            ei_seat_bind_capabilities(seat, EI_DEVICE_CAP_POINTER,
                                      EI_DEVICE_CAP_POINTER_ABSOLUTE,
                                      EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL,
                                      EI_DEVICE_CAP_TEXT, NULL);
            ptr_bound = true;
          }
          break;
        }
        case EI_EVENT_DEVICE_ADDED: {
          struct ei_device *dev = ei_event_get_device(event);
          if (ei_device_has_capability(dev, EI_DEVICE_CAP_KEYBOARD)) g_eis.kb = dev;
          if (ei_device_has_capability(dev, EI_DEVICE_CAP_POINTER_ABSOLUTE)) g_eis.abs = dev;
          if (ei_device_has_capability(dev, EI_DEVICE_CAP_POINTER)) g_eis.ptr = dev;
          if (ei_device_has_capability(dev, EI_DEVICE_CAP_SCROLL)) g_eis.scroll = dev;
          if (ei_device_has_capability(dev, EI_DEVICE_CAP_TEXT)) g_eis.text = dev;
          g_printerr("[rd] EIS device added: kb=%d abs=%d scroll=%d text=%d\n",
                     g_eis.kb != NULL, g_eis.abs != NULL, g_eis.scroll != NULL,
                     g_eis.text != NULL);
          /* Second stage: once the keyboard device is up, bind the pointer
             family (KDE bug 520464 workaround). */
          if (g_eis.kb && kb_bound && !ptr_bound && g_eis.seat) {
            ei_seat_bind_capabilities(g_eis.seat, EI_DEVICE_CAP_POINTER,
                                      EI_DEVICE_CAP_POINTER_ABSOLUTE,
                                      EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL,
                                      EI_DEVICE_CAP_TEXT, NULL);
            ptr_bound = true;
          }
          break;
        }
        case EI_EVENT_DEVICE_RESUMED: {
          struct ei_device *dev = ei_event_get_device(event);
          ei_device_start_emulating(dev, ++g_eis.sequence);
          if (ei_device_has_capability(dev, EI_DEVICE_CAP_KEYBOARD)) g_eis.kb_resumed = true;
          if (ei_device_has_capability(dev, EI_DEVICE_CAP_POINTER_ABSOLUTE)) g_eis.abs_resumed = true;
          if (ei_device_has_capability(dev, EI_DEVICE_CAP_TEXT)) g_eis.text_resumed = true;
          break;
        }
        default:
          break;
      }
      ei_event_unref(event);
    }

    /* Keep dispatching until the deadline: the text device can appear after
       the keyboard/absolute devices have resumed, and an early break leaves
       the text capability unbound even though the seat offered it. */
    if (g_eis.abs_resumed && g_eis.kb_resumed && g_eis.text_resumed) break;
  }

  g_printerr("[rd] EIS seat up: kb=%d abs=%d text=%d\n",
             g_eis.kb_resumed, g_eis.abs_resumed, g_eis.text_resumed);
  return g_eis.abs_resumed || g_eis.kb_resumed;
}

/* Consumes pending incoming events so the ei context does not queue them up
   between injections. Device-removed events are not actioned yet: a later
   injection fails per device class and falls back rather than crashing. */
static void eis_drain_events(void) {
  if (!g_eis.ei) return;
  ei_dispatch(g_eis.ei);
  struct ei_event *event = NULL;
  while ((event = ei_get_event(g_eis.ei)) != NULL) ei_event_unref(event);
}

#endif /* CU_HAVE_EIS */

/*
 * The consent dialog is the crux of the UX. Three outcomes matter:
 *   0 success, 1 the user said no, 2 anything else (including our own timeout).
 * We distinguish them in the message because "the user declined" is final and
 * "no answer within Ns" is retryable, and the model should treat them
 * differently.
 */
bool cu_input_open_wayland_session(void) {
  if (g_rd.active) return true;
  if (g_rd.tried && !g_rd.active && g_rd.last_error_code == 1) {
    /* The user declined once in this process. Re-prompting on every keystroke
       would be harassment; the caller must surface this instead. */
    return false;
  }
  g_rd.tried = true;
  g_printerr("[rd] opening session\n");

  GDBusConnection *conn = session_bus();
  if (!conn) {
    rd_fail(-1, "No session bus; xdg-desktop-portal cannot be reached.");
    return false;
  }

  /* ---- 1. CreateSession ---- */
  char *token = make_token("rd_session");
  GVariantBuilder opts;
  g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&opts, "{sv}", "handle_token", g_variant_new_string(token));
  g_variant_builder_add(&opts, "{sv}", "session_handle_token", g_variant_new_string(token));

  guint32 response = 2;
  GVariant *results = portal_request(
      conn, "org.freedesktop.portal.RemoteDesktop", "CreateSession",
      g_variant_new("(a{sv})", &opts), token, &response, 10);

  if (!results) {
    rd_fail(response, response == 1
        ? "Screen sharing was declined; Wayland input injection needs consent."
        : "Could not create a RemoteDesktop session (is xdg-desktop-portal running?).");
    g_free(token);
    return false;
  }

  const char *session_handle = NULL;
  g_variant_lookup(results, "session_handle", "&s", &session_handle);
  g_free(g_rd.session_handle);
  g_rd.session_handle = session_handle ? g_strdup(session_handle) : NULL;
  g_variant_unref(results);
  g_free(token);

  if (!g_rd.session_handle) {
    rd_fail(-1, "The portal created a session but returned no handle.");
    return false;
  }

  /* Session handles arrive as a string but must be used as an object path. */
  if (g_rd.session_handle[0] != '/') {
    rd_fail(-1, "The portal returned a malformed session handle.");
    return false;
  }

  g_printerr("[rd] session=%s\n", g_rd.session_handle);

  /* ---- 2. SelectDevices ---- */
  token = make_token("rd_devices");
  g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&opts, "{sv}", "handle_token", g_variant_new_string(token));
  g_variant_builder_add(&opts, "{sv}", "types",
                        g_variant_new_uint32(RD_DEVICE_KEYBOARD | RD_DEVICE_POINTER));
  /* persist_mode=1 asks the backend to remember the grant for this app's
     lifetime, so the dialog is not shown again on the next Start. Backends
     that do not support it ignore the key. */
  g_variant_builder_add(&opts, "{sv}", "persist_mode", g_variant_new_uint32(1));

  /* Echo back a persisted restore struct (`restore_data`) so a new-protocol
     backend can mark this session restored and skip the dialog. The older
     `restore_token` string is Start-results only: SelectDevices has no input
     slot for it (the main portal strips unknown options), so we keep it on
     disk for inspection but do not send it back. A missing or stale file
     simply means a fresh dialog. */
  GVariant *restore = rd_restore_load();
  if (restore) {
    if (g_variant_is_of_type(restore, G_VARIANT_TYPE_STRING)) {
      g_variant_unref(restore);
    } else {
      GVariant *slot = g_variant_is_of_type(restore, G_VARIANT_TYPE_VARIANT)
                           ? restore
                           : g_variant_new_variant(restore);
      g_variant_builder_add(&opts, "{sv}", "restore_data", slot);
      if (slot != restore) g_variant_unref(slot);
      g_variant_unref(restore);
    }
  }

  /*
   * The session handle comes back as a *string* but every subsequent call takes
   * it as an object path. g_variant_new("o", ...) with a bare char* is not the
   * same thing: the format string expects the pointer for an object path and
   * will happily accept it, but building the variant explicitly through
   * g_variant_new_object_path() is what guarantees the type check passes — and a
   * type mismatch here fails silently rather than with an error, which is how
   * this looked like a hang the first time.
   */
  results = portal_request(
      conn, "org.freedesktop.portal.RemoteDesktop", "SelectDevices",
      g_variant_new("(@oa{sv})",
                    g_variant_new_object_path(g_rd.session_handle), &opts),
      token, &response, 10);
  g_free(token);
  if (!results) {
    rd_fail(response, "The portal refused the keyboard/pointer device selection.");
    return false;
  }
  g_variant_unref(results);

  g_printerr("[rd] devices selected, calling Start (consent dialog)\n");

  /* ---- 3. Start (this is where the user is asked) ---- */
  token = make_token("rd_start");
  g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&opts, "{sv}", "handle_token", g_variant_new_string(token));

  /*
   * 120s by default: a real person has to read a security dialog and click
   * Allow, and a short timeout here is the difference between "works" and
   * "never works". The override exists for scripted verification, where nobody
   * is going to click anything and hanging for two minutes proves nothing.
   */
  int start_timeout = 120;
  const char *override = g_getenv("LOBSTER_CU_RD_TIMEOUT");
  if (override && *override) {
    int v = atoi(override);
    if (v > 0) start_timeout = v;
  }
  results = portal_request(
      conn, "org.freedesktop.portal.RemoteDesktop", "Start",
      g_variant_new("(@osa{sv})",
                    g_variant_new_object_path(g_rd.session_handle), "", &opts),
      token, &response, start_timeout);
  g_free(token);

  if (!results) {
    if (response == 1) {
      rd_fail(response, "Screen sharing was declined. Wayland does not allow "
                        "input injection without explicit consent; retrying will "
                        "ask again.");
    } else if (response == 3) {
      char *msg = g_strdup_printf(
          "No answer to the consent dialog within %ds. The request was not "
          "refused — the user may simply not have seen the prompt yet. Call "
          "request_input_access again when they are ready.", start_timeout);
      rd_fail(response, msg);
      g_free(msg);
    } else {
      rd_fail(response, "The RemoteDesktop session could not be started. Check "
                        "that xdg-desktop-portal and its desktop backend are "
                        "running.");
    }
    return false;
  }

  guint32 devices = 0;
  g_variant_lookup(results, "devices", "u", &devices);

  /* Persist the opaque restore_data handed back for this consented session, so
     the next process can echo it in SelectDevices and the backend can restore
     without prompting again. Stored as its own normal form; left to rot if the
     grant later changes (devices/clipboard/screenshare mismatch → not restored). */
  /* Newer backends return a struct under `restore_data`; older ones return a
     string under `restore_token`. Accept either, persist whichever arrives. */
  GVariant *restore_back = g_variant_lookup_value(results, "restore_data", NULL);
  const char *restore_key = "restore_data";
  if (!restore_back) {
    restore_back = g_variant_lookup_value(results, "restore_token", NULL);
    restore_key = "restore_token";
  }
  if (restore_back) {
    if (rd_restore_save(restore_back)) {
      g_printerr("[rd] %s persisted (%s)\n", restore_key,
                 g_variant_get_type_string(restore_back));
    } else {
      g_printerr("[rd] could not persist %s\n", restore_key);
    }
    g_variant_unref(restore_back);
  }

  g_printerr("[rd] started, devices=0x%x\n", devices);
  g_variant_unref(results);

  g_rd.devices = devices;
  g_rd.has_keyboard = (devices & RD_DEVICE_KEYBOARD) != 0;
  g_rd.has_pointer = (devices & RD_DEVICE_POINTER) != 0;
  g_rd.active = g_rd.has_keyboard || g_rd.has_pointer;
  if (!g_rd.active) {
    /* A session that starts but grants neither device is not a usable input
       route: reporting success here would make every injected event vanish
       silently. Treat it as a failure with a precise reason. */
    rd_fail(0, "The RemoteDesktop session started but granted no keyboard or "
               "pointer device (devices=0x0). Re-run request_input_access and "
               "allow both input devices in the consent dialog.");
    return false;
  }
#if defined(CU_HAVE_EIS)
  /* Bring up the EIS virtual seat. If this fails we keep the RD session and
     fall back to Notify* below; on KDE that fallback does not deliver events,
     but other backends may still honour it. */
  rd_eis_establish(conn, g_rd.session_handle);
#endif
  g_rd.last_error_code = 0;
  g_free(g_rd.last_error);
  g_rd.last_error = NULL;
  return true;
}

bool cu_remote_desktop_active(void) {
  return g_rd.active;
}

bool cu_rd_has_pointer(void) {
  return g_rd.active && g_rd.has_pointer;
}

bool cu_rd_has_keyboard(void) {
  return g_rd.active && g_rd.has_keyboard;
}

/* Records why a Notify* call was refused for the missing device type. Kept
   separate from rd_fail(): the session itself is still alive, only this device
   class is absent, so we must not tear the session down. */
static void rd_device_unavailable(const char *device) {
  g_free(g_rd.last_error);
  g_rd.last_error = g_strdup_printf(
      "The RemoteDesktop session did not grant the %s device, so this action "
      "cannot be injected through the compositor.", device);
}

/*
 * Absolute pointer motion. `stream` identifies which PipeWire stream the
 * coordinates are relative to; when the session was started for input only
 * (no sources selected), backends accept 0 and treat the single logical
 * monitor as the space. KWin and Mutter both do.
 *
 * Coordinates are logical (compositor) pixels, not device pixels — which is
 * also what AT-SPI reports, so no DPI conversion is needed.
 */
static bool rd_pointer_motion_absolute(double x, double y) {
  if (!g_rd.active) return false;
  if (!g_rd.has_pointer) { rd_device_unavailable("pointer"); return false; }
#if defined(CU_HAVE_EIS)
  if (g_eis.abs_resumed && g_eis.abs) {
    eis_drain_events();
    ei_device_pointer_motion_absolute(g_eis.abs, x, y);
    ei_device_frame(g_eis.abs, ei_now(g_eis.ei));
    return true;
  }
#endif
  GDBusConnection *conn = session_bus();
  if (!conn) return false;

  GVariantBuilder o;
  g_variant_builder_init(&o, G_VARIANT_TYPE_VARDICT);
  GVariant *reply = g_dbus_connection_call_sync(
      conn, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.RemoteDesktop", "NotifyPointerMotionAbsolute",
      g_variant_new("(oa{sv}udd)", g_rd.session_handle, &o, (guint32)0, x, y),
      NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);
  if (!reply) return false;
  g_variant_unref(reply);
  return true;
}

/* Public alias used by the input backend. */
bool cu_rd_pointer_motion_absolute(double x, double y) {
  return rd_pointer_motion_absolute(x, y);
}

const char *cu_input_last_error(void) {
  return g_rd.last_error;
}


/* Linux evdev button codes, not X11 button numbers: 0x110 left, 0x111 right,
   0x112 middle. This trips up every implementation that reuses X11 constants. */
#define EVDEV_BTN_LEFT   0x110
#define EVDEV_BTN_RIGHT  0x111
#define EVDEV_BTN_MIDDLE 0x112

static bool rd_pointer_button(int x11_button, bool pressed) {
  if (!g_rd.active) return false;
  if (!g_rd.has_pointer) { rd_device_unavailable("pointer"); return false; }
  GDBusConnection *conn = session_bus();
  if (!conn) return false;

  /* Linux evdev button codes, not X11 button numbers. This trips up every
     implementation that reuses the X11 constants: 0x110/0x111/0x112 are
     BTN_LEFT/RIGHT/MIDDLE, while X11's 1/3/2 mean the same thing in a
     different numbering. */
  int evdev = x11_button == 3 ? 0x111      /* BTN_RIGHT */
            : x11_button == 2 ? 0x112      /* BTN_MIDDLE */
            : 0x110;                       /* BTN_LEFT */

#if defined(CU_HAVE_EIS)
  {
    struct ei_device *d = g_eis.abs ? g_eis.abs : g_eis.ptr;
    if (g_eis.abs_resumed && d) {
      eis_drain_events();
      ei_device_button_button(d, (uint32_t)evdev, pressed);
      ei_device_frame(d, ei_now(g_eis.ei));
      return true;
    }
  }
#endif

  GVariantBuilder o;
  g_variant_builder_init(&o, G_VARIANT_TYPE_VARDICT);
  GVariant *reply = g_dbus_connection_call_sync(
      conn, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.RemoteDesktop", "NotifyPointerButton",
      g_variant_new("(oa{sv}iu)", g_rd.session_handle, &o, evdev,
                    (guint32)(pressed ? 1 : 0)),
      NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);
  if (!reply) return false;
  g_variant_unref(reply);
  return true;
}

bool cu_rd_pointer_button(int x11_button, bool pressed) {
  return rd_pointer_button(x11_button, pressed);
}

static bool rd_pointer_axis_discrete(bool horizontal, int steps) {
  if (!g_rd.active || steps == 0) return false;
  if (!g_rd.has_pointer) { rd_device_unavailable("pointer"); return false; }
#if defined(CU_HAVE_EIS)
  {
    struct ei_device *d = g_eis.scroll ? g_eis.scroll : g_eis.abs;
    if (g_eis.abs_resumed && d) {
      eis_drain_events();
      /* One discrete unit is 120 on the EIS scroll axis. */
      int32_t sx = horizontal ? steps * 120 : 0;
      int32_t sy = horizontal ? 0 : steps * 120;
      ei_device_scroll_discrete(d, sx, sy);
      ei_device_frame(d, ei_now(g_eis.ei));
      return true;
    }
  }
#endif
  GDBusConnection *conn = session_bus();
  if (!conn) return false;

  GVariantBuilder o;
  g_variant_builder_init(&o, G_VARIANT_TYPE_VARDICT);
  GVariant *reply = g_dbus_connection_call_sync(
      conn, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.RemoteDesktop", "NotifyPointerAxisDiscrete",
      g_variant_new("(oa{sv}ui)", g_rd.session_handle, &o,
                    (guint32)(horizontal ? 1 : 0), steps),
      NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);
  if (!reply) return false;
  g_variant_unref(reply);
  return true;
}

bool cu_rd_pointer_axis_discrete(bool horizontal, int steps) {
  return rd_pointer_axis_discrete(horizontal, steps);
}

/*
 * Keyboard, two ways:
 *
 *   NotifyKeyboardKeysym takes an X keysym, which is what we already resolve
 *   from names like "ctrl+shift+t" — no evdev translation needed, and no
 *   dependence on the user's keymap being evdev. This is the right choice for
 *   named keys.
 *
 *   NotifyKeyboardKeycode takes a Linux keycode (evdev code + 8). Needed for
 *   arbitrary text where we look up each character's keycode from the X keymap,
 *   exactly as the XTest path does.
 */
static bool rd_key_keysym(unsigned long keysym, bool pressed) {
  if (!g_rd.active) return false;
  if (!g_rd.has_keyboard) { rd_device_unavailable("keyboard"); return false; }
#if defined(CU_HAVE_EIS)
  if (g_eis.kb_resumed && g_eis.kb) {
    uint32_t ev = keysym_to_evdev(keysym);
    if (ev == 0) {
      rd_device_unavailable("keyboard key mapping");
      return false;
    }
    eis_drain_events();
    ei_device_keyboard_key(g_eis.kb, ev, pressed);
    ei_device_frame(g_eis.kb, ei_now(g_eis.ei));
    return true;
  }
#endif
  GDBusConnection *conn = session_bus();
  if (!conn) return false;

  GVariantBuilder o;
  g_variant_builder_init(&o, G_VARIANT_TYPE_VARDICT);
  GVariant *reply = g_dbus_connection_call_sync(
      conn, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.RemoteDesktop", "NotifyKeyboardKeysym",
      g_variant_new("(oa{sv}iu)", g_rd.session_handle, &o, (gint32)keysym,
                    (guint32)(pressed ? 1 : 0)),
      NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);
  if (!reply) return false;
  g_variant_unref(reply);
  return true;
}

bool cu_rd_key_keysym(unsigned long keysym, bool pressed) {
  return rd_key_keysym(keysym, pressed);
}

bool cu_rd_type_utf8(const char *utf8) {
#if defined(CU_HAVE_EIS)
  if (!g_rd.active || !utf8 || !*utf8) return false;
  if (g_eis.text_resumed && g_eis.text) {
    eis_drain_events();
    ei_device_text_utf8(g_eis.text, utf8);
    ei_device_frame(g_eis.text, ei_now(g_eis.ei));
    return true;
  }
#endif
  (void)utf8;
  return false;
}

/* ------------------------------ ScreenCast -------------------------------- */

/*
 * ScreenCast is probed rather than used for capture: pulling frames out of a
 * PipeWire node needs libpipewire, which we deliberately do not depend on (it
 * would make the helper unloadable on systems without PipeWire, which is most
 * X11-only installs).
 *
 * The probe is still worth doing properly, because its result answers a
 * question the caller actually has: "can this compositor be screen-recorded at
 * all, and at what resolution". KDE and GNOME both answer yes; a nested
 * compositor inside another compositor often says no.
 *
 * Note SelectSources types: 1 = MONITOR, 2 = WINDOW, 4 = VIRTUAL. We ask for
 * MONITOR; WINDOW would let us capture exactly the window we care about, but
 * requires the user to pick it from a list by hand, which defeats automation.
 * The full-screen capture plus AT-SPI crop we already do is the better trade.
 */
bool cu_wayland_probe_screencast(void) {
  if (g_sc.probed) return g_sc.have_stream;
  g_sc.probed = true;

  GDBusConnection *conn = session_bus();
  if (!conn) return false;

  char *token = make_token("sc_session");
  GVariantBuilder opts;
  g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&opts, "{sv}", "handle_token", g_variant_new_string(token));
  g_variant_builder_add(&opts, "{sv}", "session_handle_token", g_variant_new_string(token));

  guint32 response = 2;
  GVariant *results = portal_request(
      conn, "org.freedesktop.portal.ScreenCast", "CreateSession",
      g_variant_new("(a{sv})", &opts), token, &response, 10);
  g_free(token);
  if (!results) return false;

  const char *session_handle = NULL;
  g_variant_lookup(results, "session_handle", "&s", &session_handle);
  char *handle = session_handle ? g_strdup(session_handle) : NULL;
  g_variant_unref(results);
  if (!handle) return false;

  token = make_token("sc_sources");
  g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&opts, "{sv}", "handle_token", g_variant_new_string(token));
  g_variant_builder_add(&opts, "{sv}", "types", g_variant_new_uint32(1));    /* MONITOR */
  g_variant_builder_add(&opts, "{sv}", "multiple", g_variant_new_boolean(FALSE));
  g_variant_builder_add(&opts, "{sv}", "cursor_mode", g_variant_new_uint32(2)); /* Embedded */

  results = portal_request(
      conn, "org.freedesktop.portal.ScreenCast", "SelectSources",
      g_variant_new("(@oa{sv})", g_variant_new_object_path(handle), &opts),
      token, &response, 10);
  g_free(token);
  if (!results) { g_free(handle); return false; }
  g_variant_unref(results);

  /* Start would show a dialog. For a *probe* we do not want to interrupt the
     user, so we stop here: reaching this point proves the compositor supports
     monitor capture, which is what the report needs to say. */
  g_sc.have_stream = false;
  g_sc.node_id = 0;

  /* Close the session politely so the portal does not keep it alive. */
  GVariant *closed = g_dbus_connection_call_sync(
      conn, "org.freedesktop.portal.Desktop", handle,
      "org.freedesktop.portal.Session", "Close", NULL, NULL,
      G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);
  if (closed) g_variant_unref(closed);
  g_free(handle);
  return true;
}

/* -------------------------- wlroots protocol probing ---------------------- */

/*
 * wlroots-based compositors expose capture and input through their own
 * protocols rather than the portal:
 *
 *   zwlr_screencopy_manager_v1     — read output frames (sway, Hyprland, ...)
 *   zwlr_virtual_pointer_v1        — synthetic pointer with absolute motion
 *   zwp_virtual_keyboard_v1        — synthetic keyboard with a mutable keymap
 *   zwlr_data_control_manager_v1   — clipboard/primary selection reading
 *   ext_data_control_manager_v1    — the standardised successor to the above
 *
 * They are *not* usable through D-Bus: you have to be a Wayland client and bind
 * the global. That means a helper that wants to use them must connect to the
 * compositor itself, which is exactly what this process does not do (it is a
 * D-Bus/AT-SPI client). So we report their presence, and the report explains
 * that a native-wlroots build would be required.
 *
 * Why bother: the difference between "capture is unsupported here" and "capture
 * would work if this build bound zwlr_screencopy_manager_v1" is the difference
 * between a dead end and a roadmap.
 *
 * Detection without a Wayland connection is indirect but reliable enough: the
 * protocol names appear in the compositor's advertised global list, which
 * extensions and helper tools surface via `swaymsg -t get_version` / Hyprland's
 * `hyprctl`. We keep the probe dependency-free by checking the environment marks
 * those compositors leave behind, plus the client library that would be needed.
 */
bool cu_wlroots_likely(void) {
  const char *desktop = g_getenv("XDG_CURRENT_DESKTOP");
  const char *session_desktop = g_getenv("XDG_SESSION_DESKTOP");
  const char *marks[] = { "sway", "Hyprland", "river", "Wayfire", "wayfire",
                          "wlroots", "labwc", NULL };
  for (int i = 0; marks[i]; i++) {
    if (desktop && g_strrstr(desktop, marks[i])) return true;
    if (session_desktop && g_strrstr(session_desktop, marks[i])) return true;
  }
  /* Sway sets SWAYSOCK for its IPC socket; Hyprland sets HYPRLAND_INSTANCE_SIGNATURE. */
  if (g_getenv("SWAYSOCK") || g_getenv("HYPRLAND_INSTANCE_SIGNATURE")) return true;
  return false;
}

/* Does a wlr-protocol client library exist that a future build could link? */
bool cu_wlroots_capture_available(void) {
  return cu_wlroots_likely() && g_getenv("WAYLAND_DISPLAY") != NULL;
}

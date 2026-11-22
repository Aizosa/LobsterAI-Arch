/*
 * Screen capture.
 *
 * Four backends, tried in an order chosen per desktop and session type. The
 * order is not cosmetic: on a composited session the fast path usually fails, so
 * trying it first costs an instant, while getting it wrong the other way round
 * would mean showing a consent dialog for something we could have done silently.
 *
 *   1. org.gnome.Shell.Screenshot — GNOME only, both X11 and Wayland. Silent,
 *      prompt-free, and able to crop in the compositor. This is the best path
 *      anywhere on Linux, so it goes first when present.
 *
 *   2. XGetImage on the window — silent and instantaneous, but only succeeds
 *      where the compositor does not redirect that window's pixels. On X11
 *      with a non-compositing WM (xfwm4, openbox, i3) this is the normal case.
 *      Under KWin/Mutter the root window is fully redirected through Composite
 *      and XGetImage returns BadMatch: the framebuffer is simply not there.
 *
 *   3. xdg-desktop-portal Screenshot — works on Wayland and XWayland, and is
 *      the only thing that works at all on a composited session. It asks the
 *      compositor, which shows its screenshot UI the first time. With the
 *      permission remembered (the "remember" checkbox) KDE and GNOME answer
 *      silently thereafter.
 *
 *   4. xfce4-screenshooter — XFCE's own capture tool. Needed because
 *      xdg-desktop-portal has no XFCE backend, so Screenshot falls through to
 *      the GTK backend where it may be limited or missing entirely.
 *
 * A window is never captured by clipping the root window, because the root
 * window cannot be read on a composited session. Instead the full screen is
 * captured and cropped to the rect AT-SPI reported for the window.
 */
#define _GNU_SOURCE
#include "cu.h"

#include <cairo/cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <poll.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#if defined(CU_HAVE_X11)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

/* Set by whichever backend last failed, for check_permissions to report. */
static char *g_capture_error = NULL;

static void capture_set_error(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void capture_set_error(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  g_free(g_capture_error);
  g_capture_error = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  g_debug("[capture] %s", g_capture_error);
}

const char *cu_capture_last_error(void) {
  return g_capture_error;
}

/* ------------------------------- lifecycle ------------------------------- */

void cu_image_free(CuImage *img) {
  if (!img) return;
  g_free(img->pixels);
  g_free(img);
}

/* ----------------------------- PNG encoding ----------------------------- */

typedef struct {
  unsigned char *data;
  gsize len;
  gsize cap;
} PngBuffer;

static cairo_status_t png_write_cb(void *closure, const unsigned char *data,
                                   unsigned int length) {
  PngBuffer *buf = closure;
  if (buf->len + length > buf->cap) {
    gsize next = buf->cap ? buf->cap : 65536;
    while (next < buf->len + length) next *= 2;
    unsigned char *grown = g_realloc(buf->data, next);
    if (!grown) return CAIRO_STATUS_NO_MEMORY;
    buf->data = grown;
    buf->cap = next;
  }
  memcpy(buf->data + buf->len, data, length);
  buf->len += length;
  return CAIRO_STATUS_SUCCESS;
}

unsigned char *cu_image_to_png(const CuImage *img, gsize *out_len) {
  if (out_len) *out_len = 0;
  if (!img || !img->pixels || img->width <= 0 || img->height <= 0) return NULL;

  cairo_surface_t *surface =
      cairo_image_surface_create(CAIRO_FORMAT_RGB24, img->width, img->height);
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface);
    return NULL;
  }

  unsigned char *dst = cairo_image_surface_get_data(surface);
  if (!dst) {
    cairo_surface_destroy(surface);
    return NULL;
  }
  int dst_stride = cairo_image_surface_get_stride(surface);
  /* cairo RGB24 packs BGRx in memory on little-endian targets. */
  for (int y = 0; y < img->height; y++) {
    const unsigned char *src = img->pixels + (size_t)y * img->stride;
    unsigned char *row = dst + (size_t)y * dst_stride;
    for (int x = 0; x < img->width; x++) {
      row[x * 4 + 0] = src[x * 3 + 2];
      row[x * 4 + 1] = src[x * 3 + 1];
      row[x * 4 + 2] = src[x * 3 + 0];
      row[x * 4 + 3] = 0xFF;
    }
  }
  cairo_surface_mark_dirty(surface);

  PngBuffer png = { NULL, 0, 0 };
  cairo_status_t st = cairo_surface_write_to_png_stream(surface, png_write_cb, &png);
  cairo_surface_destroy(surface);

  if (st != CAIRO_STATUS_SUCCESS || !png.data) {
    g_free(png.data);
    return NULL;
  }
  if (out_len) *out_len = png.len;
  return png.data;
}

char *cu_image_to_png_base64(const CuImage *img) {
  gsize len = 0;
  unsigned char *png = cu_image_to_png(img, &len);
  if (!png) return NULL;
  char *b64 = g_base64_encode(png, len);
  g_free(png);
  return b64;
}

/* ------------------------------- utilities ------------------------------- */

static CuImage *image_new(int w, int h) {
  if (w <= 0 || h <= 0) return NULL;
  CuImage *img = g_new0(CuImage, 1);
  img->width = w;
  img->height = h;
  img->stride = w * 3;
  img->pixels = g_malloc0((gsize)img->stride * h);
  return img;
}

/* Copies src[rect] into a new image, clamped to the source bounds. */
static CuImage *image_crop(const CuImage *src, int x, int y, int w, int h) {
  if (!src || !src->pixels) return NULL;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > src->width) w = src->width - x;
  if (y + h > src->height) h = src->height - y;
  if (w <= 0 || h <= 0) return NULL;

  CuImage *out = image_new(w, h);
  for (int row = 0; row < h; row++) {
    memcpy(out->pixels + (size_t)row * out->stride,
           src->pixels + (size_t)(y + row) * src->stride + (size_t)x * 3,
           (size_t)w * 3);
  }
  return out;
}

/* --------------------------------- X11 ---------------------------------- */

#if defined(CU_HAVE_X11)

static int g_x_error = 0;
static int x_error_handler(Display *dpy, XErrorEvent *ev) {
  (void)dpy;
  g_x_error = ev->error_code;
  return 0;
}

/*
 * Reads one window's own pixels. Only valid when the compositor does not
 * redirect that window; the caller must be prepared for NULL.
 *
 * Every X call below is wrapped so a stale window id (the app closed between
 * list_windows and the capture) yields NULL instead of aborting the process:
 * Xlib's default error handler calls exit() on BadWindow, and stdout would die
 * with it, taking the whole JSON-RPC session down.
 */
static CuImage *x11_capture_window_pixels(Window win) {
  Display *dpy = XOpenDisplay(NULL);
  if (!dpy) return NULL;

  g_x_error = 0;
  XErrorHandler previous = XSetErrorHandler(x_error_handler);

  XWindowAttributes attr;
  Status ok = XGetWindowAttributes(dpy, win, &attr);
  XSync(dpy, False);
  if (!ok || g_x_error || attr.map_state != IsViewable || attr.width <= 0 ||
      attr.height <= 0) {
    XSetErrorHandler(previous);
    XCloseDisplay(dpy);
    return NULL;
  }

  g_x_error = 0;
  XImage *src = XGetImage(dpy, win, 0, 0, (unsigned)attr.width,
                          (unsigned)attr.height, AllPlanes, ZPixmap);
  XSync(dpy, False);
  XSetErrorHandler(previous);

  if (!src || g_x_error) {
    if (src) XDestroyImage(src);
    XCloseDisplay(dpy);
    return NULL;
  }

  CuImage *img = image_new(attr.width, attr.height);
  /* Respect the actual channel masks instead of assuming RGB byte order:
     XWayland on a 10-bit or BGR visual would otherwise come out colour-swapped. */
  Visual *vis = attr.visual;
  for (int y = 0; y < img->height; y++) {
    unsigned char *row = img->pixels + (size_t)y * img->stride;
    for (int x = 0; x < img->width; x++) {
      unsigned long p = XGetPixel(src, x, y);
      unsigned char r = 0, g = 0, b = 0;
      if (vis && vis->class == TrueColor) {
        r = (unsigned char)(((p & vis->red_mask) >> 16) & 0xFF);
        g = (unsigned char)(((p & vis->green_mask) >> 8) & 0xFF);
        b = (unsigned char)((p & vis->blue_mask) & 0xFF);
      } else {
        r = (unsigned char)((p >> 16) & 0xFF);
        g = (unsigned char)((p >> 8) & 0xFF);
        b = (unsigned char)(p & 0xFF);
      }
      row[x * 3 + 0] = r;
      row[x * 3 + 1] = g;
      row[x * 3 + 2] = b;
    }
  }

  XDestroyImage(src);
  XCloseDisplay(dpy);
  return img;
}

#else

static CuImage *x11_capture_window_pixels(Window win) {
  (void)win;
  return NULL;
}

#endif /* CU_HAVE_X11 */

/* ----------------------- KWin ScreenShot2 (KDE Wayland) ----------------------- */

/*
 * KWin's private, restricted-interface DBus service. Unlike every other
 * backend here it hands back raw pixels over a pipe rather than a PNG file:
 * we put the write end of a pipe in a GUnixFDList, KWin writes the QImage bits
 * into it and closes it, and the reply dictionary carries width/height/stride
 * /format.
 *
 * The service is permission-gated: KWin resolves our pid back to its .desktop
 * file and only allows interfaces listed in X-KDE-DBUS-Restricted-Interfaces.
 * A non-whitelisted caller receives an Error.NoAuthorized reply; this backend
 * then returns NULL so the portal fallback can run.
 */

#define CU_SS2_SERVICE "org.kde.KWin.ScreenShot2"
#define CU_SS2_PATH    "/org/kde/KWin/ScreenShot2"
#define CU_SS2_IFACE   "org.kde.KWin.ScreenShot2"

/* Qt6 QImage::Format values we may have to decode. Packed QRgb formats
 * (RGB32/ARGB32) are 0x??RRGGBB, little-endian memory B,G,R,(A/x); the
 * byte-ordered formats (RGBX8888/RGBA8888) lay R,G,B,(X/A) down verbatim. */
#define CU_QIMAGE_RGB32        4
#define CU_QIMAGE_ARGB32       5
#define CU_QIMAGE_ARGB32_PREMULT 6
#define CU_QIMAGE_RGBX8888    16
#define CU_QIMAGE_RGBA8888    17
#define CU_QIMAGE_RGBA8888_PREMULT 18

static bool cu_ss2_available(void) {
  return cu_bus_name_has_owner(CU_SS2_SERVICE);
}

/* Read every byte the compositor wrote into the pipe until it closes the
 * write end. Polls with a timeout so a dead KWin cannot block the session. */
static GByteArray *ss2_read_pipe(int fd) {
  GByteArray *buf = g_byte_array_new();
  unsigned char chunk[65536];
  while (true) {
    struct pollfd pfd = { fd, POLLIN, 0 };
    int ready = poll(&pfd, 1, 10000);
    if (ready < 0) {
      if (errno == EINTR) continue;
      g_byte_array_free(buf, TRUE);
      return NULL;
    }
    if (ready == 0) {
      g_debug("[ss2] timed out reading screenshot pipe");
      g_byte_array_free(buf, TRUE);
      return NULL;
    }
    if (!(pfd.revents & POLLIN)) break;
    ssize_t n = read(fd, chunk, sizeof(chunk));
    if (n > 0) {
      g_byte_array_append(buf, chunk, (guint)n);
    } else if (n == 0) {
      break; /* EOF: writer closed its end */
    } else {
      if (errno == EINTR) continue;
      g_byte_array_free(buf, TRUE);
      return NULL;
    }
  }
  return buf;
}

/* Convert the raw QImage bits into packed RGB, honouring the pixel layout the
 * reply reported. The byte-ordered RGBX/RGBA formats keep R,G,B in memory
 * order; the packed RGB32/ARGB32 formats are little-endian B,G,R,(A/x). */
static CuImage *ss2_decode(const GByteArray *raw, guint32 width, guint32 height,
                            guint32 stride, guint32 format) {
  if (!raw || raw->len == 0 || width == 0 || height == 0) return NULL;
  CuImage *img = image_new((int)width, (int)height);
  bool byte_ordered = (format == CU_QIMAGE_RGBX8888 ||
                       format == CU_QIMAGE_RGBA8888 ||
                       format == CU_QIMAGE_RGBA8888_PREMULT);
  for (guint32 y = 0; y < height; y++) {
    const unsigned char *row = raw->data + (gsize)y * stride;
    unsigned char *out = img->pixels + (gsize)y * img->stride;
    for (guint32 x = 0; x < width; x++) {
      const unsigned char *px = row + (gsize)x * 4;
      unsigned char r, g, b;
      if (byte_ordered) { r = px[0]; g = px[1]; b = px[2]; }
      else              { b = px[0]; g = px[1]; r = px[2]; }
      out[x * 3 + 0] = r;
      out[x * 3 + 1] = g;
      out[x * 3 + 2] = b;
    }
  }
  return img;
}

static CuImage *screenshot2_capture_fullscreen(void) {
  GError *err = NULL;
  GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!conn) {
    if (err) g_error_free(err);
    capture_set_error("No session bus; KWin ScreenShot2 is unreachable.");
    return NULL;
  }

  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) {
    capture_set_error("Could not create screenshot pipe: %s", strerror(errno));
    g_object_unref(conn);
    return NULL;
  }

  GUnixFDList *fd_list = g_unix_fd_list_new();
  int fd_index = g_unix_fd_list_append(fd_list, fds[1], NULL);
  if (fd_index < 0) {
    capture_set_error("Could not hand the screenshot pipe to KWin.");
    close(fds[0]); close(fds[1]);
    g_object_unref(fd_list); g_object_unref(conn);
    return NULL;
  }

  GVariantBuilder opts;
  g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&opts, "{sv}", "include-cursor",
                        g_variant_new_boolean(FALSE));

  GVariant *reply = g_dbus_connection_call_with_unix_fd_list_sync(
      conn, CU_SS2_SERVICE, CU_SS2_PATH, CU_SS2_IFACE, "CaptureActiveScreen",
      g_variant_new("(a{sv}h)", &opts, fd_index),
      G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 10000,
      fd_list, NULL, NULL, &err);

  close(fds[1]);
  g_object_unref(fd_list);

  if (!reply) {
    capture_set_error("KWin ScreenShot2 call failed: %s",
                      err ? err->message : "unknown error");
    if (err) g_error_free(err);
    close(fds[0]);
    g_object_unref(conn);
    return NULL;
  }

  /* The reply body is a one-element tuple wrapping the results dictionary. */
  GVariant *body = g_variant_get_child_value(reply, 0);
  guint32 width = 0, height = 0, stride = 0, format = 0;
  g_variant_lookup(body, "width",  "u", &width);
  g_variant_lookup(body, "height", "u", &height);
  g_variant_lookup(body, "stride", "u", &stride);
  g_variant_lookup(body, "format", "u", &format);
  g_variant_unref(body);
  g_variant_unref(reply);

  GByteArray *raw = ss2_read_pipe(fds[0]);
  close(fds[0]);
  g_object_unref(conn);

  if (!raw || width == 0 || height == 0) {
    capture_set_error("KWin ScreenShot2 returned no image data.");
    if (raw) g_byte_array_free(raw, TRUE);
    return NULL;
  }
  if (stride == 0) stride = width * 4;

  CuImage *img = ss2_decode(raw, width, height, stride, format);
  g_byte_array_free(raw, TRUE);
  if (!img) capture_set_error("KWin ScreenShot2 image decode failed.");
  return img;
}

/* --------------------------- Wayland via portal --------------------------- */

/*
 * The portal's Screenshot method is asynchronous even though its signature is
 * not: it returns a request handle, then emits org.freedesktop.portal.Request
 * .Response on that object once the compositor has saved the PNG. We run a
 * private GMainContext for the duration of one call so the signal is delivered
 * without hijacking whichever context the caller uses.
 *
 * The sandboxed-portal protocol also allows pre-selecting the returned handle
 * by sending "handle_token"; we do that so there is no window in which the
 * response could be emitted before we subscribe.
 */

typedef struct {
  GMainLoop *loop;
  int response;
  char *uri;
  bool timed_out;
} PortalWait;

static void on_portal_response(GDBusConnection *conn, const char *sender,
                               const char *object_path, const char *interface,
                               const char *signal_name, GVariant *params,
                               gpointer user_data) {
  (void)conn; (void)sender; (void)object_path;
  (void)interface; (void)signal_name;
  PortalWait *w = user_data;

  guint32 response = 1;
  GVariant *results = NULL;
  g_variant_get(params, "(u@a{sv})", &response, &results);
  w->response = (int)response;

  if (w->response == 0 && results) {
    /* v2 (default): "uri". v3+: "uri" is gone, only "uris" (as). */
    if (g_variant_lookup(results, "uri", "s", &w->uri)) {
      /* taken */
    } else {
      GVariant *uris = g_variant_lookup_value(results, "uris", G_VARIANT_TYPE_STRING_ARRAY);
      if (uris) {
        gsize n = g_variant_n_children(uris);
        if (n > 0) g_variant_get_child(uris, 0, "s", &w->uri);
        g_variant_unref(uris);
      }
    }
  }
  if (results) g_variant_unref(results);
  g_main_loop_quit(w->loop);
}

/* Loads a PNG from disk (the portal hands back a file:// URI). */
static CuImage *load_png_file(const char *path) {
  cairo_surface_t *surface = cairo_image_surface_create_from_png(path);
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface);
    return NULL;
  }
  cairo_surface_flush(surface);

  int w = cairo_image_surface_get_width(surface);
  int h = cairo_image_surface_get_height(surface);
  if (w <= 0 || h <= 0) {
    cairo_surface_destroy(surface);
    return NULL;
  }

  /* The PNG is ARGB32; flatten onto black so transparency from a composited
     capture does not turn into surprising white blobs downstream. */
  cairo_surface_t *rgb = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
  cairo_t *cr = cairo_create(rgb);
  cairo_set_source_rgb(cr, 0, 0, 0);
  cairo_paint(cr);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  cairo_surface_flush(rgb);

  unsigned char *data = cairo_image_surface_get_data(rgb);
  int stride = cairo_image_surface_get_stride(rgb);
  if (!data) {
    cairo_surface_destroy(rgb);
    return NULL;
  }

  CuImage *img = image_new(w, h);
  for (int y = 0; y < h; y++) {
    unsigned char *row = img->pixels + (size_t)y * img->stride;
    const unsigned char *src_row = data + (size_t)y * stride;
    for (int x = 0; x < w; x++) {
      /* cairo native endian ARGB32: B,G,R,A in memory on little-endian. */
      row[x * 3 + 0] = src_row[x * 4 + 2];
      row[x * 3 + 1] = src_row[x * 4 + 1];
      row[x * 3 + 2] = src_row[x * 4 + 0];
    }
  }
  cairo_surface_destroy(rgb);
  return img;
}

/* Small random token so concurrent helpers do not collide on one request path. */
static char *make_token(void) {
  return g_strdup_printf("lobster_%u_%u", (unsigned)g_random_int(), (unsigned)g_random_int());
}

/* The compositor can leave its dialog open indefinitely; end the wait instead. */
static gboolean portal_timeout_cb(gpointer data) {
  PortalWait *w = data;
  w->timed_out = true;
  g_main_loop_quit(w->loop);
  return G_SOURCE_REMOVE;
}

/* How long we wait for the Screenshot Response before treating the portal as
   unresponsive. Distinct from a user declining: no answer is retryable, a
   refusal is final for that grant. */
#define CU_PORTAL_CAPTURE_TIMEOUT_SECONDS 25

/*
 * Grabs the whole screen through the portal and hands back the *path* of the
 * PNG the compositor wrote, leaving decoding and cleanup to the caller.
 *
 * Returning a path rather than pixels keeps this function free of image code,
 * which matters because three of the four backends produce a file: GNOME Shell
 * and xfce4-screenshooter both write one too, and routing all three through a
 * single take_png_file() is what stops the "forgot to unlink" bug from being
 * reintroduced per backend.
 *
 * Returns NULL when the portal is absent (no xdg-desktop-portal installed, or a
 * headless session) or when the user declines.
 */
static bool portal_capture_screen(char **out_path) {
  *out_path = NULL;
  GError *err = NULL;
  GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!conn) {
    if (err) {
      g_debug("[capture] no session bus: %s", err->message);
      g_error_free(err);
    }
    capture_set_error("No session bus; xdg-desktop-portal is unreachable.");
    return NULL;
  }

  char *token = make_token();
  /* Predict the request object name: the portal uses the sender's unique name
     with ':' and '.' replaced by '_', plus our token. Subscribing before the
     call means the Response signal can never be missed. */
  const char *sender = g_dbus_connection_get_unique_name(conn);
  if (!sender) { g_free(token); g_object_unref(conn); return NULL; }
  GString *sanitized = g_string_new(sender + 1);
  for (gchar *p = sanitized->str; *p; p++) {
    if (*p == '.') *p = '_';
  }
  char *request_path = g_strdup_printf(
      "/org/freedesktop/portal/desktop/request/%s/%s", sanitized->str, token);

  PortalWait wait = { g_main_loop_new(NULL, FALSE), 1, NULL, false };
  guint sub = g_dbus_connection_signal_subscribe(
      conn, "org.freedesktop.portal.Desktop",
      "org.freedesktop.portal.Request", "Response",
      request_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
      on_portal_response, &wait, NULL);

  /* Timeout guard: the compositor may leave its dialog unanswered, in which
     case we must not block the request thread forever. */
  guint timer = g_timeout_add_seconds_full(
      G_PRIORITY_DEFAULT, CU_PORTAL_CAPTURE_TIMEOUT_SECONDS,
      portal_timeout_cb, &wait, NULL);

  GVariantBuilder opts;
  g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&opts, "{sv}", "handle_token", g_variant_new_string(token));
  g_variant_builder_add(&opts, "{sv}", "interactive", g_variant_new_boolean(FALSE));
  g_variant_builder_add(&opts, "{sv}", "modal", g_variant_new_boolean(FALSE));

  GVariant *reply = g_dbus_connection_call_sync(
      conn,
      "org.freedesktop.portal.Desktop",
      "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.Screenshot",
      "Screenshot",
      g_variant_new("(sa{sv})", "", &opts),
      G_VARIANT_TYPE("(o)"),
      G_DBUS_CALL_FLAGS_NONE,
      5000, NULL, &err);

  if (!reply) {
    if (err) {
      g_debug("[capture] portal Screenshot call failed: %s", err->message);
      g_error_free(err);
    }
    g_source_remove(timer);
    g_dbus_connection_signal_unsubscribe(conn, sub);
    g_main_loop_unref(wait.loop);
    g_free(request_path); g_string_free(sanitized, TRUE);
    g_free(token); g_object_unref(conn);
    return NULL;
  }
  g_variant_unref(reply);

  /* Block until Response arrives or the timeout fires. The signal subscription
     is what drives the quit; there is nothing to poll. */
  if (wait.response == 1) {
    g_main_loop_run(wait.loop);
  }

  g_source_remove(timer);
  g_dbus_connection_signal_unsubscribe(conn, sub);
  g_main_loop_unref(wait.loop);

  char *result_path = NULL;
  if (wait.response == 0 && wait.uri) {
    result_path = g_filename_from_uri(wait.uri, NULL, NULL);
    if (!result_path && g_str_has_prefix(wait.uri, "file://")) {
      result_path = g_strdup(wait.uri + 7);
    }
    /* The compositor wrote this file into ~/Pictures; the caller owns it now and
       is expected to delete it, which take_png_file() does. */
  } else if (wait.timed_out) {
    capture_set_error("The Screenshot portal did not answer within %ds "
                      "(request %s): no Response signal arrived and no consent "
                      "dialog appeared. The backend may be hung; retry, or use "
                      "the X11/XWayland capture path.",
                      CU_PORTAL_CAPTURE_TIMEOUT_SECONDS, request_path);
  } else if (wait.response == 1) {
    capture_set_error("The screen capture request was declined. Grant it once "
                      "through the system dialog (ticking \"remember\") to make "
                      "later captures silent.");
  } else if (wait.response != 0) {
    capture_set_error("The screen capture portal returned an error "
                      "(response=%d, request %s).",
                      (int)wait.response, request_path);
  } else {
    capture_set_error("The portal reported success but returned no file URI "
                      "(request %s).", request_path);
  }

  g_free(wait.uri);
  g_free(request_path);
  g_string_free(sanitized, TRUE);
  g_free(token);
  g_object_unref(conn);

  *out_path = result_path;
  return result_path != NULL;
}

/* ----------------------------- portal capture ----------------------------- */

/*
 * Loads whatever file a backend produced and deletes it. Every backend writes a
 * PNG to a path it chose; this is the one place that turns a path into pixels,
 * so cleanup cannot be forgotten on one of the four paths.
 */
static CuImage *take_png_file(char *path) {
  if (!path) return NULL;
  CuImage *img = load_png_file(path);
  unlink(path);
  g_free(path);
  return img;
}

/* ------------------------------- public API ------------------------------- */

/*
 * Order of attempts, and why:
 *
 *   GNOME Shell first — where it exists it is silent, prompt-free and crops in
 *   the compositor, which beats everything else on every axis.
 *
 *   Then XGetImage on the window — instantaneous and silent. It fails on a
 *   composited session (BadMatch), but when it fails it fails in microseconds,
 *   so trying it is free.
 *
 *   Then KWin ScreenShot2 — silent compositor capture on KDE Wayland; gated by
 *   the desktop whitelist, and returns NULL to fall through when refused.
 *
 *   Then the portal — the portable path. May show a dialog the first time.
 *
 *   Then the desktop's own tool (currently only XFCE has one that works
 *   non-interactively).
 */
CuImage *cu_capture_window_rect(unsigned long x11_window, int x, int y, int w,
                                int h) {
  g_free(g_capture_error);
  g_capture_error = NULL;

  bool have_rect = (w > 0 && h > 0);

  /* 1. GNOME Shell. ScreenshotArea crops for us, so no full-screen frame is
        ever allocated; with no rect, ask for the whole screen. */
  if (cu_gnome_screenshot_path_available()) {
    char *path = have_rect
        ? cu_gnome_screenshot_to_file(x, y, w, h, false)
        : cu_gnome_screenshot_to_file(0, 0, 0, 0, false);
    CuImage *img = take_png_file(path);
    if (img) return img;
    /* Not fatal: fall through and let the portal try. */
  }

  /* 2. Direct read of a specific window's pixels. Only when we know the X id;
        AT-SPI ids are hashes and must never be passed here. */
#if defined(CU_HAVE_X11)
  if (x11_window != 0) {
    CuImage *direct = x11_capture_window_pixels((Window)x11_window);
    if (direct) return direct;
  }
#endif

  /* 2.5 KWin ScreenShot2 — silent compositor capture on KDE Wayland. Fails
        safe: an unauthorized or absent service returns NULL and the portal
        below still runs. */
  if (cu_session_is_wayland() && cu_ss2_available()) {
    CuImage *screen = screenshot2_capture_fullscreen();
    if (screen) {
      if (!have_rect) return screen;
      CuImage *cropped = image_crop(screen, x, y, w, h);
      if (cropped) {
        cu_image_free(screen);
        return cropped;
      }
      return screen;
    }
  }

  /* 3. Portal. Captures the full screen; we crop to the AT-SPI rect. */
  char *portal_path = NULL;
  if (portal_capture_screen(&portal_path)) {
    CuImage *screen = take_png_file(portal_path);
    if (screen) {
      if (!have_rect) return screen;
      CuImage *cropped = image_crop(screen, x, y, w, h);
      if (cropped) {
        cu_image_free(screen);
        return cropped;
      }
      /* Rect outside the screen (a window straddling two monitors, or stale
         geometry): hand back the full capture rather than nothing. */
      return screen;
    }
  }

  /* 4. Desktop-specific command-line fallback. */
  if (cu_have_command("xfce4-screenshooter")) {
    char *path = cu_xfce_screenshot_to_file(x, y, w, h);
    if (path) {
      CuImage *full = take_png_file(path);
      if (full) {
        if (!have_rect) return full;
        CuImage *cropped = image_crop(full, x, y, w, h);
        if (cropped) {
          cu_image_free(full);
          return cropped;
        }
        return full;
      }
    }
  }

  if (!g_capture_error) {
    capture_set_error("No capture backend produced an image. On Wayland this "
                      "usually means the screenshot permission was declined; on "
                      "X11 it means the window is gone.");
  }
  return NULL;
}

CuImage *cu_capture_window(int x, int y, int w, int h) {
  return cu_capture_window_rect(0, x, y, w, h);
}

/* Reads a portal interface's `version` property; 0 when it is not exported. */
int cu_portal_version(const char *interface) {
  GError *err = NULL;
  GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!conn) {
    if (err) g_error_free(err);
    return 0;
  }
  GVariant *reply = g_dbus_connection_call_sync(
      conn, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
      "org.freedesktop.DBus.Properties", "Get",
      g_variant_new("(ss)", interface, "version"),
      G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err);
  g_object_unref(conn);
  if (!reply) {
    if (err) g_error_free(err);
    return 0;
  }
  /*
   * "(v)" already unwraps the outer variant: the value we get back here *is*
   * the property's type, not another variant around it. Calling
   * g_variant_get_variant() on it would be a double-unwrap and would trip a
   * GLib critical while silently yielding 0.
   */
  GVariant *v = NULL;
  g_variant_get(reply, "(v)", &v);
  int version = 0;
  if (v) {
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32))
      version = (int)g_variant_get_uint32(v);
    g_variant_unref(v);
  }
  g_variant_unref(reply);
  return version;
}

/*
 * Which backend a capture will actually use, in the order the dispatcher tries
 * them. Reported in check_permissions so the model knows whether to expect a
 * dialog before it calls get_window_state.
 */
const char *cu_capture_preferred_backend(void) {
  if (cu_gnome_screenshot_path_available()) return "gnome-shell";
  if (cu_session_is_wayland()) {
    if (cu_ss2_available()) return "kwin-screenshot2";
    if (cu_wlroots_likely()) return "wlroots-screencopy";
    if (cu_portal_version("org.freedesktop.portal.Screenshot") > 0) return "portal";
    if (cu_have_command("xfce4-screenshooter")) return "xfce4-screenshooter";
    return "none";
  }
  if (cu_x11_available()) return "x11-window-pixels";
  if (cu_portal_version("org.freedesktop.portal.Screenshot") > 0) return "portal";
  return "none";
}

bool cu_capture_available(void) {
  /* A backend existing is not the same as it answering, but it is the only
     thing we can check without triggering a dialog. The dispatcher reports
     real failures through cu_capture_last_error(). */
  return strcmp(cu_capture_preferred_backend(), "none") != 0;
}

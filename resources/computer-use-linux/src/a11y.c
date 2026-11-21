#include "cu.h"

#include <atspi/atspi.h>
#include <gio/gio.h>
#include <string.h>

/*
 * AT-SPI2 backend.
 *
 * The tree shape is: desktop -> application -> window(FRAME/DIALOG) -> ...
 * We walk it with a node budget, assign each visited node a stable index, and
 * remember the mapping index -> (accessible, path) so a later click/set_value
 * can jump straight back to the same element.
 *
 * Element indices are only valid together with a state_id, exactly like the
 * macOS helper: get_window_state returns a fresh state_id, and any action
 * carrying an older state_id is rejected by the MCP layer before we see it.
 */

struct _CuA11y {
  bool ready;
  /* state_id -> GPtrArray<AtspiAccessible*> in outline order. */
  GHashTable *states;
  /* window id -> AtspiAccessible* (borrowed from the desktop tree). */
  GHashTable *windows;
  GMutex lock;
};

/* ---------------------------------------------------------------- utils */

static const char *role_str(AtspiAccessible *obj) {
  AtspiRole role = atspi_accessible_get_role(obj, NULL);
  const char *name = atspi_role_get_name(role);
  return name ? name : "unknown";
}

static char *safe_name(AtspiAccessible *obj) {
  char *name = atspi_accessible_get_name(obj, NULL);
  if (!name) name = g_strdup("");
  /* Collapse newlines so the outline stays one-line-per-node. */
  for (char *p = name; *p; p++) {
    if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
  }
  return name;
}

/* The accessible's on-screen rectangle in screen coordinates. AT-SPI >= 2.5
   exposes geometry through the Component interface rather than on the
   accessible itself. */
static bool obj_rect(AtspiAccessible *obj, AtspiCoordType type,
                     int *x, int *y, int *w, int *h) {
  AtspiComponent *comp = atspi_accessible_get_component_iface(obj);
  if (!comp) return false;
  GError *err = NULL;
  AtspiRect *rect = atspi_component_get_extents(comp, type, &err);
  g_object_unref(comp);
  if (!rect) {
    if (err) g_error_free(err);
    return false;
  }
  *x = rect->x; *y = rect->y; *w = rect->width; *h = rect->height;
  g_free(rect);
  return true;
}

static int obj_child_count(AtspiAccessible *obj) {
  GError *err = NULL;
  int n = atspi_accessible_get_child_count(obj, &err);
  if (err) g_error_free(err);
  return n < 0 ? 0 : n;
}

static AtspiAccessible *obj_child(AtspiAccessible *obj, int i) {
  GError *err = NULL;
  AtspiAccessible *child = atspi_accessible_get_child_at_index(obj, i, &err);
  if (err) g_error_free(err);
  return child;
}

/* ------------------------------------------------------------- lifecycle */

/*
 * Whether this process actually attached to the accessibility bus.
 *
 * Distinct from cu_atspi_bus_reachable(): that asks the session bus whether
 * at-spi2-core is running, which can be true while atspi_init() still fails for
 * this process — a missing AT_SPI_BUS_ADDRESS, a sandbox that blocks the socket,
 * or a startup race. When attached is false, list_windows returns an empty list
 * and every window action fails, so the report has to say so.
 */
static bool g_atspi_attached = false;
void cu_atspi_set_attached(bool attached);
void cu_atspi_set_attached(bool attached) { g_atspi_attached = attached; }
bool cu_atspi_attached(void) { return g_atspi_attached; }

CuA11y *cu_a11y_new(void) {
  CuA11y *a = g_new0(CuA11y, 1);
  if (atspi_init() != 0) {
    g_free(a);
    return NULL;
  }
  a->ready = true;
  cu_atspi_set_attached(true);
  a->states = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                    (GDestroyNotify)g_ptr_array_unref);
  a->windows = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);
  g_mutex_init(&a->lock);
  return a;
}

void cu_a11y_free(CuA11y *a) {
  if (!a) return;
  cu_atspi_set_attached(false);
  if (a->states) g_hash_table_destroy(a->states);
  if (a->windows) g_hash_table_destroy(a->windows);
  g_mutex_clear(&a->lock);
  atspi_exit();
  g_free(a);
}

bool cu_a11y_ready(CuA11y *a) { return a && a->ready; }


/*
 * Whether the AT-SPI bus is reachable at all, independent of any CuA11y
 * instance. check_permissions runs before the first window call, so it needs to
 * answer this without initialising the tree (which would be a wasted bus
 * connection if the answer is no).
 *
 * The accessibility bus is itself a D-Bus connection whose address the session
 * bus hands out through org.a11y.Bus.GetAddress. Asking for that address is the
 * canonical liveness check: it fails when at-spi2-core is not running, which is
 * exactly the state that produces an empty window list.
 */
bool cu_atspi_bus_reachable(void) {
  GError *err = NULL;
  GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!conn) {
    if (err) g_error_free(err);
    return false;
  }
  GVariant *reply = g_dbus_connection_call_sync(
      conn, "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress",
      NULL, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err);
  g_object_unref(conn);
  if (!reply) {
    if (err) g_error_free(err);
    return false;
  }
  const char *address = NULL;
  g_variant_get(reply, "(&s)", &address);
  bool ok = address != NULL && *address != '\0';
  g_variant_unref(reply);
  return ok;
}

/* ------------------------------------------------------------- discovery */

/*
 * Walks the whole desktop once, collecting every window-like node with its
 * owning application. Window ids are the AT-SPI accessible's object path hashed
 * to a stable int, which is what the MCP layer round-trips back to us.
 */
typedef struct {
  AtspiAccessible *app;
  char *app_name;
  int pid;
} AppOwner;

static bool is_window_role(const char *role) {
  return strcmp(role, "frame") == 0 || strcmp(role, "dialog") == 0 ||
         strcmp(role, "window") == 0 || strcmp(role, "alert") == 0;
}

/*
 * Stable window id.
 *
 * AT-SPI 2.5x no longer exposes the D-Bus object path, so we derive the id from
 * the window's identity as the toolkit reports it: its process id, its role, and
 * its title. That is stable across list calls within a session (the only span an
 * id needs to survive) and distinct between two windows of the same app that
 * have different titles.
 */
static long long obj_window_id(AtspiAccessible *obj) {
  int pid = atspi_accessible_get_process_id(obj, NULL);
  char *name = safe_name(obj);
  const char *role = role_str(obj);
  char *key = g_strdup_printf("%d|%s|%s", pid, role, name);
  guint64 h = g_str_hash(key);
  g_free(key);
  g_free(name);
  /* Keep it positive and inside 31 bits so it round-trips through JavaScript. */
  return (long long)(h & 0x7fffffff);
}

static void collect_windows(CuA11y *a, AtspiAccessible *desktop, CuJson *j) {
  int apps = obj_child_count(desktop);
  cu_json_arr_begin(j);
  for (int i = 0; i < apps; i++) {
    AtspiAccessible *app = obj_child(desktop, i);
    if (!app) continue;
    char *app_name = safe_name(app);
    int pid = atspi_accessible_get_process_id(app, NULL);
    int wins = obj_child_count(app);

    /* Some toolkits report the frame as a child of the app; others report the
       app itself as the frame. Handle both. */
    bool app_is_itself_a_window = is_window_role(role_str(app));

    for (int w = 0; w < wins; w++) {
      AtspiAccessible *win = obj_child(app, w);
      if (!win) continue;
      const char *role = role_str(win);
      if (!is_window_role(role)) {
        g_object_unref(win);
        continue;
      }
      long long wid = obj_window_id(win);
      char *title = safe_name(win);
      int x = 0, y = 0, cw = 0, ch = 0;
      bool has_rect = obj_rect(win, ATSPI_COORD_TYPE_SCREEN, &x, &y, &cw, &ch);
      /* Resolve the X11 counterpart now, while we still have pid/title/app, so
         later EWMH and XGetImage/XTest calls do not have to re-match a window
         that may have moved or changed title. 0 = no X11 route (native Wayland
         client, or a toolkit that publishes neither pid nor a matching class). */
      unsigned long xwin = cu_x11_find_window(pid, title, app_name);

      g_hash_table_insert(a->windows, GINT_TO_POINTER((int)wid), win);

      cu_json_obj_begin(j);
      cu_json_key(j, "id"); cu_json_int(j, wid);
      cu_json_key(j, "app"); cu_json_str(j, app_name);
      cu_json_key(j, "title"); cu_json_str(j, title);
      cu_json_key(j, "role"); cu_json_str(j, role);
      cu_json_key(j, "pid"); cu_json_int(j, pid);
      if (xwin != 0) { cu_json_key(j, "x11_window"); cu_json_int(j, (long long)xwin); }
      cu_json_key(j, "onScreen");
      cu_json_bool(j, has_rect && cw > 0 && ch > 0);
      cu_json_key_obj(j, "bounds");
      cu_json_key(j, "x"); cu_json_int(j, has_rect ? x : CU_INVALID_COORD);
      cu_json_key(j, "y"); cu_json_int(j, has_rect ? y : CU_INVALID_COORD);
      cu_json_key(j, "width"); cu_json_int(j, cw);
      cu_json_key(j, "height"); cu_json_int(j, ch);
      cu_json_obj_end(j);
      cu_json_obj_end(j);

      g_free(title);
      /* win stays in the table; do not unref. */
    }

    if (app_is_itself_a_window) {
      long long wid = obj_window_id(app);
      char *title = safe_name(app);
      int x = 0, y = 0, cw = 0, ch = 0;
      bool has_rect = obj_rect(app, ATSPI_COORD_TYPE_SCREEN, &x, &y, &cw, &ch);
      unsigned long xwin = cu_x11_find_window(pid, title, app_name);
      g_hash_table_insert(a->windows, GINT_TO_POINTER((int)wid), app);
      cu_json_obj_begin(j);
      cu_json_key(j, "id"); cu_json_int(j, wid);
      cu_json_key(j, "app"); cu_json_str(j, app_name);
      cu_json_key(j, "title"); cu_json_str(j, title);
      cu_json_key(j, "role"); cu_json_str(j, role_str(app));
      cu_json_key(j, "pid"); cu_json_int(j, pid);
      if (xwin != 0) { cu_json_key(j, "x11_window"); cu_json_int(j, (long long)xwin); }
      cu_json_key(j, "onScreen"); cu_json_bool(j, has_rect && cw > 0 && ch > 0);
      cu_json_key_obj(j, "bounds");
      cu_json_key(j, "x"); cu_json_int(j, has_rect ? x : CU_INVALID_COORD);
      cu_json_key(j, "y"); cu_json_int(j, has_rect ? y : CU_INVALID_COORD);
      cu_json_key(j, "width"); cu_json_int(j, cw);
      cu_json_key(j, "height"); cu_json_int(j, ch);
      cu_json_obj_end(j);
      cu_json_obj_end(j);
      g_free(title);
    }

    g_free(app_name);
    g_object_unref(app);
  }
  cu_json_arr_end(j);
}

char *cu_a11y_list_windows(CuA11y *a) {
  if (!cu_a11y_ready(a)) return NULL;
  AtspiAccessible *desktop = atspi_get_desktop(0);
  if (!desktop) return NULL;

  g_mutex_lock(&a->lock);
  g_hash_table_remove_all(a->windows);

  CuJson j; cu_json_init(&j);
  cu_json_obj_begin(&j);
  cu_json_key(&j, "windows");
  collect_windows(a, desktop, &j);
  cu_json_obj_end(&j);

  char *out = g_strdup(j.buf->str);
  cu_json_free(&j);
  g_mutex_unlock(&a->lock);

  g_object_unref(desktop);
  return out;
}

char *cu_a11y_list_apps(CuA11y *a, bool include_installed) {
  if (!cu_a11y_ready(a)) return NULL;
  AtspiAccessible *desktop = atspi_get_desktop(0);
  if (!desktop) return NULL;

  g_mutex_lock(&a->lock);
  g_hash_table_remove_all(a->windows);

  CuJson j; cu_json_init(&j);
  cu_json_obj_begin(&j);
  cu_json_key_arr(&j, "running");

  int apps = obj_child_count(desktop);
  for (int i = 0; i < apps; i++) {
    AtspiAccessible *app = obj_child(desktop, i);
    if (!app) continue;
    char *app_name = safe_name(app);
    if (!*app_name) { g_free(app_name); g_object_unref(app); continue; }
    int pid = atspi_accessible_get_process_id(app, NULL);

    cu_json_obj_begin(&j);
    cu_json_key(&j, "app"); cu_json_str(&j, app_name);
    cu_json_key(&j, "pid"); cu_json_int(&j, pid);
    cu_json_key_arr(&j, "windows");
    int wins = obj_child_count(app);
    for (int w = 0; w < wins; w++) {
      AtspiAccessible *win = obj_child(app, w);
      if (!win) continue;
      const char *role = role_str(win);
      if (!is_window_role(role)) { g_object_unref(win); continue; }
      long long wid = obj_window_id(win);
      g_hash_table_insert(a->windows, GINT_TO_POINTER((int)wid), win);
      char *title = safe_name(win);
      unsigned long xwin = cu_x11_find_window(pid, title, app_name);
      cu_json_obj_begin(&j);
      cu_json_key(&j, "id"); cu_json_int(&j, wid);
      cu_json_key(&j, "app"); cu_json_str(&j, app_name);
      cu_json_key(&j, "title"); cu_json_str(&j, title);
      cu_json_key(&j, "pid"); cu_json_int(&j, pid);
      if (xwin != 0) { cu_json_key(&j, "x11_window"); cu_json_int(&j, (long long)xwin); }
      cu_json_obj_end(&j);
      g_free(title);
    }
    cu_json_arr_end(&j);
    cu_json_obj_end(&j);
    g_free(app_name);
    g_object_unref(app);
  }
  cu_json_arr_end(&j);

  /* Installed apps come from .desktop entries, which is what launch_app uses. */
  if (include_installed) {
    cu_json_key_arr(&j, "installed");
    /* Filled by desktop_entry_scan() in cu_desktop.c; kept out of this module
       so the accessibility path has no dependency on the XDG dirs. */
    extern void cu_desktop_list_installed(CuJson *j);
    cu_desktop_list_installed(&j);
    cu_json_arr_end(&j);
  }

  cu_json_obj_end(&j);
  char *out = g_strdup(j.buf->str);
  cu_json_free(&j);
  g_mutex_unlock(&a->lock);
  g_object_unref(desktop);
  return out;
}

/* ------------------------------------------------------------- outline */

typedef struct {
  GString *tree;      /* the human/model-facing outline text */
  GPtrArray *index;   /* index -> AtspiAccessible* */
  int budget;
  int emitted;
  int text_total;
  int text_limit;
  GString *document;  /* concatenated readable text, largest-first */
} OutlineCtx;

/* Appends `count` spaces. */
static void indent_into(GString *out, int depth) {
  for (int i = 0; i < depth; i++) g_string_append(out, "  ");
}

/*
 * One outline line:
 *
 *   [12] button "Open" @640,320
 *
 * The shape is the contract the MCP layer documents to the model ("[index] role
 * \"label\" ...; @x,y = center in screenshot pixels"), so it is produced here
 * rather than assembled in TypeScript from a structured array.
 */
static void outline_node(OutlineCtx *ctx, AtspiAccessible *obj, int depth) {
  if (ctx->emitted >= ctx->budget) return;

  char *name = safe_name(obj);
  const char *role = role_str(obj);
  int x = 0, y = 0, w = 0, h = 0;
  bool has_rect = obj_rect(obj, ATSPI_COORD_TYPE_WINDOW, &x, &y, &w, &h);

  int idx = ctx->emitted++;
  g_ptr_array_add(ctx->index, obj);

  indent_into(ctx->tree, depth);
  g_string_append_printf(ctx->tree, "[%d] %s", idx, role);
  if (*name) {
    g_string_append(ctx->tree, " \"");
    /* Newlines were already collapsed by safe_name; quotes would break the
       one-line-per-node contract, so they become single quotes. */
    for (const char *p = name; *p; p++) {
      g_string_append_c(ctx->tree, *p == '"' ? '\'' : *p);
    }
    g_string_append_c(ctx->tree, '"');
  }
  if (has_rect && w > 0 && h > 0) {
    g_string_append_printf(ctx->tree, " @%d,%d", x + w / 2, y + h / 2);
  }
  g_string_append_c(ctx->tree, '\n');

  /* Collect readable text. Editor and document roles carry the content the
     model actually needs, so they come out into document_text rather than the
     tree, matching how the macOS helper reports document_text. */
  if (ctx->text_total < ctx->text_limit) {
    AtspiText *text = atspi_accessible_get_text_iface(obj);
    if (text) {
      GError *err = NULL;
      char *body = atspi_text_get_text(text, 0, -1, &err);
      if (err) { g_error_free(err); err = NULL; }
      if (body && *body) {
        int room = ctx->text_limit - ctx->text_total;
        int len = (int)strlen(body);
        if (len > room) len = room;
        if (len > 0) {
          if (ctx->document->len > 0) g_string_append_c(ctx->document, '\n');
          g_string_append_len(ctx->document, body, len);
          ctx->text_total += len;
        }
      }
      g_free(body);
      g_object_unref(text);
    }
  }

  g_free(name);

  if (depth < 24) {
    int children = obj_child_count(obj);
    for (int i = 0; i < children && ctx->emitted < ctx->budget; i++) {
      AtspiAccessible *child = obj_child(obj, i);
      if (!child) continue;
      outline_node(ctx, child, depth + 1);
      g_object_unref(child);
    }
  }
}

static AtspiAccessible *lookup_window(CuA11y *a, long long window_id) {
  return g_hash_table_lookup(a->windows, GINT_TO_POINTER((int)window_id));
}

/*
 * Resolves a window id, re-walking the desktop if the cache has no entry.
 *
 * The MCP layer may hand us an id it obtained from list_windows in an earlier
 * turn, and an id is derived from the window's live identity (pid|role|title),
 * so re-walking reproduces the same id. Without this a second call in the same
 * session would spuriously fail with "window not found" whenever the cache was
 * populated by a different request.
 */
static AtspiAccessible *lookup_window_rescan(CuA11y *a, long long window_id) {
  AtspiAccessible *win = lookup_window(a, window_id);
  if (win) return win;

  AtspiAccessible *desktop = atspi_get_desktop(0);
  if (!desktop) return NULL;
  /* collect_windows both registers ids and ignores its JSON sink, so a throwaway
     writer is enough to warm the cache. */
  CuJson scratch; cu_json_init(&scratch);
  cu_json_arr_begin(&scratch);
  collect_windows(a, desktop, &scratch);
  cu_json_arr_end(&scratch);
  cu_json_free(&scratch);
  g_object_unref(desktop);

  return lookup_window(a, window_id);
}

bool cu_a11y_find_window(CuA11y *a, long long window_id, char **app_name,
                         char **title, int *pid) {
  if (!cu_a11y_ready(a)) return false;
  g_mutex_lock(&a->lock);
  AtspiAccessible *win = lookup_window_rescan(a, window_id);
  if (!win) {
    g_mutex_unlock(&a->lock);
    return false;
  }
  if (title) *title = safe_name(win);
  if (pid) *pid = atspi_accessible_get_process_id(win, NULL);
  if (app_name) {
    AtspiAccessible *parent = atspi_accessible_get_parent(win, NULL);
    if (parent) {
      *app_name = safe_name(parent);
      g_object_unref(parent);
    } else {
      *app_name = g_strdup("");
    }
  }
  g_mutex_unlock(&a->lock);
  return true;
}

char *cu_a11y_window_state(CuA11y *a, long long window_id, const char *app,
                           bool want_screenshot, bool want_text, int max_nodes,
                           int text_limit) {
  if (!cu_a11y_ready(a)) return NULL;
  if (max_nodes <= 0) max_nodes = CU_MAX_NODES_DEFAULT;
  if (text_limit <= 0) text_limit = CU_MAX_TEXT_DEFAULT;

  g_mutex_lock(&a->lock);
  AtspiAccessible *win = lookup_window_rescan(a, window_id);
  if (!win) {
    g_mutex_unlock(&a->lock);
    return NULL;
  }

  /* Monotonic per-process counter; the lock already serialises callers, so a
     plain increment is sufficient and avoids aliasing a guint64 as gint. */
  static guint64 state_seq = 0;
  state_seq++;
  char state_id[64];
  g_snprintf(state_id, sizeof(state_id), "s%llu-%lld",
             (unsigned long long)state_seq, window_id);

  CuJson j; cu_json_init(&j);
  cu_json_obj_begin(&j);
  cu_json_key(&j, "state_id"); cu_json_str(&j, state_id);

  char *title = safe_name(win);
  int win_pid = atspi_accessible_get_process_id(win, NULL);
  unsigned long xwin = cu_x11_find_window(win_pid, title, app);

  cu_json_key_obj(&j, "window");
  cu_json_key(&j, "id"); cu_json_int(&j, window_id);
  if (app) { cu_json_key(&j, "app"); cu_json_str(&j, app); }
  cu_json_key(&j, "title"); cu_json_str(&j, title);
  cu_json_key(&j, "pid"); cu_json_int(&j, win_pid);
  if (xwin != 0) { cu_json_key(&j, "x11_window"); cu_json_int(&j, (long long)xwin); }
  cu_json_obj_end(&j);
  g_free(title);

  /* Screenshot: the window's own framebuffer is unreadable on a composited
     session, so we ask for the rect AT-SPI reports and let capture crop the
     full-screen grab down to it. */
  if (want_screenshot) {
    int wx = 0, wy = 0, ww = 0, wh = 0;
    bool have_rect = obj_rect(win, ATSPI_COORD_TYPE_SCREEN, &wx, &wy, &ww, &wh);
    CuImage *img = have_rect && ww > 0 && wh > 0
                       ? cu_capture_window_rect(xwin, wx, wy, ww, wh)
                       : cu_capture_window_rect(xwin, 0, 0, 0, 0);
    char *b64 = img ? cu_image_to_png_base64(img) : NULL;
    if (b64) {
      char shot_id[64];
      g_snprintf(shot_id, sizeof(shot_id), "shot-%s", state_id);
      cu_json_key_arr(&j, "screenshots");
      cu_json_obj_begin(&j);
      cu_json_key(&j, "id"); cu_json_str(&j, shot_id);
      cu_json_key(&j, "width"); cu_json_int(&j, img->width);
      cu_json_key(&j, "height"); cu_json_int(&j, img->height);
      /*
       * The screenshot is cropped to the window, so a pixel the model reads off
       * it is window-relative. originX/originY carry the screen coordinate of
       * pixel (0,0) so the MCP layer can translate back before injecting input.
       * pointWidth/pointHeight equal the pixel size: Linux has no logical point
       * space separate from the framebuffer, unlike a Retina Mac.
       */
      cu_json_key(&j, "originX"); cu_json_int(&j, have_rect ? wx : 0);
      cu_json_key(&j, "originY"); cu_json_int(&j, have_rect ? wy : 0);
      cu_json_key(&j, "pointWidth"); cu_json_int(&j, img->width);
      cu_json_key(&j, "pointHeight"); cu_json_int(&j, img->height);
      cu_json_key(&j, "scaleX"); cu_json_int(&j, 1);
      cu_json_key(&j, "scaleY"); cu_json_int(&j, 1);
      cu_json_key(&j, "coordinateSpace"); cu_json_str(&j, "screenshot_pixels");
      /* The MCP layer emits data: URLs to its image content, so hand it one. */
      char *url = g_strdup_printf("data:image/png;base64,%s", b64);
      cu_json_key(&j, "url"); cu_json_str(&j, url);
      g_free(url);
      cu_json_obj_end(&j);
      cu_json_arr_end(&j);
      g_free(b64);
    } else {
      /* "No screenshot" and "capture failed" need different model reactions, so
         report the reason the capture backend recorded instead of a generic
         line. When nothing was recorded (e.g. no backend at all), fall back to
         the generic guidance. */
      const char *why = cu_capture_last_error();
      cu_json_key(&j, "screenshot_error");
      cu_json_str(&j, (why && *why) ? why
                      : "Screen capture unavailable: no backend produced an image. "
                        "On Wayland this usually means the screenshot permission "
                        "was declined; on X11 it means the window is gone.");
    }
    if (img) cu_image_free(img);
  }

  /* Outline */
  if (want_text) {
    OutlineCtx ctx = { g_string_new(NULL), NULL, max_nodes, 0, 0, text_limit,
                       g_string_new(NULL) };
    ctx.index = g_ptr_array_new();
    outline_node(&ctx, win, 0);

    cu_json_key_obj(&j, "accessibility");
    cu_json_key(&j, "tree"); cu_json_str(&j, ctx.tree->str);
    if (ctx.document->len > 0) {
      cu_json_key(&j, "document_text"); cu_json_str(&j, ctx.document->str);
    }
    cu_json_key(&j, "element_count"); cu_json_int(&j, ctx.emitted);
    cu_json_key(&j, "truncated"); cu_json_bool(&j, ctx.emitted >= max_nodes);
    cu_json_obj_end(&j);

    /* Remember the index -> accessible mapping under this state_id. */
    GPtrArray *stored = g_ptr_array_new_with_free_func(g_object_unref);
    for (guint i = 0; i < ctx.index->len; i++) {
      /* index holds borrowed refs; take our own so the array owns them. */
      g_ptr_array_add(stored, g_object_ref(g_ptr_array_index(ctx.index, i)));
    }
    g_hash_table_insert(a->states, g_strdup(state_id), stored);
    g_ptr_array_unref(ctx.index);
    g_string_free(ctx.tree, TRUE);
    g_string_free(ctx.document, TRUE);
  }

  cu_json_obj_end(&j);
  char *out = g_strdup(j.buf->str);
  cu_json_free(&j);
  g_mutex_unlock(&a->lock);
  return out;
}

/* -------------------------------------------------------------- actions */

static AtspiAccessible *resolve_element(CuA11y *a, const char *state_id,
                                        int element_index) {
  if (!state_id) return NULL;
  GPtrArray *arr = g_hash_table_lookup(a->states, state_id);
  if (!arr) return NULL;
  if (element_index < 0 || (guint)element_index >= arr->len) return NULL;
  return g_ptr_array_index(arr, element_index);
}

char *cu_a11y_click_element(CuA11y *a, const char *state_id, int element_index) {
  if (!cu_a11y_ready(a)) return NULL;
  g_mutex_lock(&a->lock);
  AtspiAccessible *el = resolve_element(a, state_id, element_index);
  if (!el) {
    g_mutex_unlock(&a->lock);
    return NULL;
  }
  AtspiAction *action = atspi_accessible_get_action_iface(el);
  if (!action) {
    g_mutex_unlock(&a->lock);
    return NULL;
  }

  /* Prefer an action literally named "click"; otherwise take the first one,
     which is the toolkit's default (press/activate/select as the case may be). */
  GError *err = NULL;
  int n = atspi_action_get_n_actions(action, &err);
  if (err) { g_error_free(err); err = NULL; }
  gboolean ok = FALSE;
  int target = -1;
  for (int i = 0; i < n; i++) {
    char *nm = atspi_action_get_action_name(action, i, NULL);
    if (nm && g_ascii_strcasecmp(nm, "click") == 0) {
      target = i;
      g_free(nm);
      break;
    }
    g_free(nm);
  }
  if (target < 0 && n > 0) target = 0;
  if (target >= 0) {
    ok = atspi_action_do_action(action, target, &err);
    if (err) g_error_free(err);
  }
  g_object_unref(action);
  g_mutex_unlock(&a->lock);
  return ok ? g_strdup("Click completed.") : NULL;
}

char *cu_a11y_set_value(CuA11y *a, const char *state_id, int element_index,
                        const char *value) {
  if (!cu_a11y_ready(a)) return NULL;
  g_mutex_lock(&a->lock);
  AtspiAccessible *el = resolve_element(a, state_id, element_index);
  if (!el) {
    g_mutex_unlock(&a->lock);
    return NULL;
  }

  gboolean ok = FALSE;

  /* 1. Editable text is the precise route: it replaces the buffer in place. */
  AtspiEditableText *editable = atspi_accessible_get_editable_text_iface(el);
  if (editable) {
    GError *err = NULL;
    ok = atspi_editable_text_set_text_contents(editable, value, &err);
    if (err) g_error_free(err);
    g_object_unref(editable);
  }

  /* 2. Check boxes and toggle buttons take a checked state rather than text. */
  if (!ok) {
    AtspiAction *action = atspi_accessible_get_action_iface(el);
    if (action) {
      AtspiRole role = atspi_accessible_get_role(el, NULL);
      bool wants_on = value && (g_ascii_strcasecmp(value, "true") == 0 ||
                                g_ascii_strcasecmp(value, "1") == 0 ||
                                g_ascii_strcasecmp(value, "on") == 0 ||
                                g_ascii_strcasecmp(value, "checked") == 0);
      const char *target = NULL;
      if (wants_on) {
        target = "check";
      } else if (value && (g_ascii_strcasecmp(value, "false") == 0 ||
                           g_ascii_strcasecmp(value, "0") == 0 ||
                           g_ascii_strcasecmp(value, "off") == 0 ||
                           g_ascii_strcasecmp(value, "unchecked") == 0)) {
        target = "uncheck";
      }
      if (target && (role == ATSPI_ROLE_CHECK_BOX || role == ATSPI_ROLE_TOGGLE_BUTTON ||
                     role == ATSPI_ROLE_RADIO_BUTTON)) {
        int count = atspi_action_get_n_actions(action, NULL);
        for (int i = 0; i < count; i++) {
          char *aname = atspi_action_get_action_name(action, i, NULL);
          if (aname && g_ascii_strcasecmp(aname, target) == 0) {
            GError *err = NULL;
            ok = atspi_action_do_action(action, i, &err);
            if (err) g_error_free(err);
            g_free(aname);
            break;
          }
          g_free(aname);
        }
      }
      g_object_unref(action);
    }
  }

  g_mutex_unlock(&a->lock);
  return g_strdup(ok ? "Value set completed." : NULL);
}

char *cu_a11y_perform_action(CuA11y *a, const char *state_id, int element_index,
                             const char *action) {
  if (!cu_a11y_ready(a)) return NULL;
  g_mutex_lock(&a->lock);
  AtspiAccessible *el = resolve_element(a, state_id, element_index);
  if (!el) {
    g_mutex_unlock(&a->lock);
    return NULL;
  }
  /* AT-SPI actions are addressed by index, not name; map the AX names the MCP
     layer uses onto the action interface's declared actions. */
  AtspiAction *act = atspi_accessible_get_action_iface(el);
  if (!act) {
    g_mutex_unlock(&a->lock);
    return NULL;
  }
  int count = atspi_action_get_n_actions(act, NULL);
  gboolean ok = FALSE;
  for (int i = 0; i < count; i++) {
    char *aname = atspi_action_get_action_name(act, i, NULL);
    if (aname && action &&
        (g_ascii_strcasecmp(aname, action) == 0 ||
         (g_str_has_prefix(action, "AX") &&
          g_ascii_strcasecmp(aname, action + 2) == 0))) {
      GError *err = NULL;
      ok = atspi_action_do_action(act, i, &err);
      if (err) g_error_free(err);
      g_free(aname);
      break;
    }
    g_free(aname);
  }
  g_object_unref(act);
  g_mutex_unlock(&a->lock);
  return g_strdup(ok ? "Secondary action completed." : NULL);
}

/* ----------------------------------------------------------- close window */

/*
 * Closing a window has no AT-SPI method. The portable X11 route is a
 * WM_DELETE_WINDOW client message, which is exactly what clicking the close
 * button does — the app decides whether to honour it, so unsaved work still
 * prompts. On pure Wayland there is no such channel; we report that instead of
 * killing the process, which would discard the user's data.
 */
char *cu_a11y_close_window(CuA11y *a, long long window_id) {
  if (!cu_a11y_ready(a)) return NULL;
  g_mutex_lock(&a->lock);
  AtspiAccessible *win = lookup_window_rescan(a, window_id);
  g_mutex_unlock(&a->lock);
  if (!win) return NULL;

  /* Try the window's own "close" action first; Qt and GTK expose it on frames. */
  AtspiAction *act = atspi_accessible_get_action_iface(win);
  if (act) {
    int n = atspi_action_get_n_actions(act, NULL);
    for (int i = 0; i < n; i++) {
      char *nm = atspi_action_get_action_name(act, i, NULL);
      if (nm && (g_ascii_strcasecmp(nm, "close") == 0 ||
                 g_ascii_strcasecmp(nm, "delete") == 0)) {
        GError *err = NULL;
        gboolean ok = atspi_action_do_action(act, i, &err);
        if (err) g_error_free(err);
        g_free(nm);
        g_object_unref(act);
        if (ok) return g_strdup("Window closed.");
        return NULL;
      }
      g_free(nm);
    }
    g_object_unref(act);
  }

  const char *session = g_getenv("XDG_SESSION_TYPE");
  if (session && strcmp(session, "wayland") == 0) {
    return g_strdup(
        "This window does not expose a close action, and Wayland has no "
        "cross-client close request. Use the app's own UI (ctrl+q) instead.");
  }
  return NULL;
}

char *cu_a11y_activate_window(CuA11y *a, long long window_id) {
  if (!cu_a11y_ready(a)) return NULL;
  g_mutex_lock(&a->lock);
  AtspiAccessible *win = lookup_window_rescan(a, window_id);
  if (!win) {
    g_mutex_unlock(&a->lock);
    return NULL;
  }
  GError *err = NULL;
  gboolean ok = FALSE;

  /* 1. The toolkit's own focus request. Qt and GTK forward this to the
        compositor, which may or may not honour it. */
  AtspiComponent *comp = atspi_accessible_get_component_iface(win);
  if (comp) {
    ok = atspi_component_grab_focus(comp, &err);
    if (err) { g_error_free(err); err = NULL; }
    g_object_unref(comp);
  }

  /* 2. The AT-SPI "raise" action, which some toolkits expose on frames. */
  if (!ok) {
    AtspiAction *act = atspi_accessible_get_action_iface(win);
    if (act) {
      int n = atspi_action_get_n_actions(act, NULL);
      for (int i = 0; i < n; i++) {
        char *nm = atspi_action_get_action_name(act, i, NULL);
        if (nm && (g_ascii_strcasecmp(nm, "raise") == 0 ||
                   g_ascii_strcasecmp(nm, "activate") == 0)) {
          ok = atspi_action_do_action(act, i, NULL);
          g_free(nm);
          if (ok) break;
          continue;
        }
        g_free(nm);
      }
      g_object_unref(act);
    }
  }
  g_mutex_unlock(&a->lock);

  if (ok) return g_strdup("Window activated.");

  /*
   * 3. Neither worked. Under a Wayland compositor that is expected: focus is
   * decided by the compositor and an application may not pull it. Report that
   * precisely instead of returning a generic failure, so the model knows to ask
   * the user to click rather than retrying blindly.
   */
  const char *session = g_getenv("XDG_SESSION_TYPE");
  if (session && strcmp(session, "wayland") == 0) {
    return g_strdup(
        "The compositor refused the focus request. On Wayland a client cannot "
        "raise another window; input still reaches the window if it is already "
        "visible, otherwise ask the user to bring it to the front.");
  }
  return NULL;
}

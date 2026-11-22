/*
 * KDE plasma-window-management enumeration fallback.
 *
 * On a KDE Wayland session AT-SPI can miss native clients, so this module
 * binds the compositor's private window-management global and mirrors the
 * windows it reports into the same JSON shape the AT-SPI path produces.
 *
 * The protocol is a desktop implementation detail ("regular clients must not
 * use this protocol"), and only one client may bind it at a time. Everything
 * here is defensive: if the display, the global, or the bind is missing the
 * function appends nothing and the AT-SPI list stands as-is.
 *
 * Compiled only when wayland-client headers are present; otherwise this file
 * reduces to a no-op stub.
 */
#include "cu.h"

#if defined(CU_HAVE_WAYLAND_CLIENT)

#include <wayland-client.h>
#include "plasma-window-management-client-protocol.h"

/* state bits from org_kde_plasma_window_management.state */
#define CU_PW_STATE_ACTIVE    0x1u
#define CU_PW_STATE_MINIMIZED 0x2u

typedef struct {
  char *uuid;
  char *title;
  char *app_id;
  int pid;
  int x, y, width, height;
  bool has_geometry;
  bool unmapped;
  uint32_t state;
  struct org_kde_plasma_window *win;
} PlasmaWindow;

typedef struct {
  struct wl_display *display;
  struct wl_registry *registry;
  struct org_kde_plasma_window_management *wm;
  GPtrArray *windows;
} PlasmaCtx;

static void pw_free(PlasmaWindow *pw) {
  if (!pw) return;
  g_free(pw->uuid);
  g_free(pw->title);
  g_free(pw->app_id);
  if (pw->win) org_kde_plasma_window_destroy(pw->win);
  g_free(pw);
}

/* ------------------------- org_kde_plasma_window events ------------------------- */

static void on_title(void *data, struct org_kde_plasma_window *win,
                     const char *title) {
  PlasmaWindow *pw = org_kde_plasma_window_get_user_data(win);
  (void)data;
  if (!pw) return;
  g_free(pw->title);
  pw->title = g_strdup(title ? title : "");
}

static void on_app_id(void *data, struct org_kde_plasma_window *win,
                      const char *app_id) {
  PlasmaWindow *pw = org_kde_plasma_window_get_user_data(win);
  (void)data;
  if (!pw) return;
  g_free(pw->app_id);
  pw->app_id = g_strdup(app_id ? app_id : "");
}

static void on_state(void *data, struct org_kde_plasma_window *win, uint32_t flags) {
  PlasmaWindow *pw = org_kde_plasma_window_get_user_data(win);
  (void)data;
  if (pw) pw->state = flags;
}

static void on_pid(void *data, struct org_kde_plasma_window *win, uint32_t pid) {
  PlasmaWindow *pw = org_kde_plasma_window_get_user_data(win);
  (void)data;
  if (pw) pw->pid = (int)pid;
}

static void on_geometry(void *data, struct org_kde_plasma_window *win, int32_t x,
                        int32_t y, uint32_t width, uint32_t height) {
  PlasmaWindow *pw = org_kde_plasma_window_get_user_data(win);
  (void)data;
  if (!pw) return;
  pw->x = x;
  pw->y = y;
  pw->width = (int)width;
  pw->height = (int)height;
  pw->has_geometry = true;
}

static void on_unmapped(void *data, struct org_kde_plasma_window *win) {
  PlasmaWindow *pw = org_kde_plasma_window_get_user_data(win);
  (void)data;
  if (pw) pw->unmapped = true;
}

/* Uninteresting events still need a non-NULL callback: the Wayland client
 * library aborts the process if an event arrives with its listener unset. */
static void pw_noop_win(void *data, struct org_kde_plasma_window *win) {
  (void)data; (void)win;
}
static void pw_noop_int(void *data, struct org_kde_plasma_window *win, int32_t n) {
  (void)data; (void)win; (void)n;
}
static void pw_noop_str(void *data, struct org_kde_plasma_window *win,
                         const char *s) {
  (void)data; (void)win; (void)s;
}
static void pw_noop_parent(void *data, struct org_kde_plasma_window *win,
                           struct org_kde_plasma_window *parent) {
  (void)data; (void)win; (void)parent;
}
static void pw_noop_menu(void *data, struct org_kde_plasma_window *win,
                         const char *service, const char *path) {
  (void)data; (void)win; (void)service; (void)path;
}
static void pw_noop_geo(void *data, struct org_kde_plasma_window *win, int32_t x,
                        int32_t y, uint32_t width, uint32_t height) {
  (void)data; (void)win; (void)x; (void)y; (void)width; (void)height;
}

static const struct org_kde_plasma_window_listener win_listener = {
  .title_changed = on_title,
  .app_id_changed = on_app_id,
  .state_changed = on_state,
  .virtual_desktop_changed = pw_noop_int,
  .themed_icon_name_changed = pw_noop_str,
  .unmapped = on_unmapped,
  .initial_state = pw_noop_win,
  .parent_window = pw_noop_parent,
  .geometry = on_geometry,
  .icon_changed = pw_noop_win,
  .pid_changed = on_pid,
  .virtual_desktop_entered = pw_noop_str,
  .virtual_desktop_left = pw_noop_str,
  .application_menu = pw_noop_menu,
  .activity_entered = pw_noop_str,
  .activity_left = pw_noop_str,
  .resource_name_changed = pw_noop_str,
  .client_geometry = pw_noop_geo,
  .mapped = pw_noop_win,
  .resource_class_changed = pw_noop_str,
};

/* -------------------- org_kde_plasma_window_management events -------------------- */

static void on_window_with_uuid(void *data,
                                struct org_kde_plasma_window_management *wm,
                                uint32_t id, const char *uuid) {
  PlasmaCtx *ctx = data;
  (void)id;
  PlasmaWindow *pw = g_new0(PlasmaWindow, 1);
  pw->uuid = g_strdup(uuid);
  pw->win = org_kde_plasma_window_management_get_window_by_uuid(wm, uuid);
  if (pw->win) {
    org_kde_plasma_window_set_user_data(pw->win, pw);
    org_kde_plasma_window_add_listener(pw->win, &win_listener, pw);
  }
  g_ptr_array_add(ctx->windows, pw);
}

static void wm_noop_state(void *data, struct org_kde_plasma_window_management *wm,
                           uint32_t state) {
  (void)data; (void)wm; (void)state;
}
static void wm_noop_id(void *data, struct org_kde_plasma_window_management *wm,
                        uint32_t id) {
  (void)data; (void)wm; (void)id;
}
static void wm_noop_array(void *data, struct org_kde_plasma_window_management *wm,
                          struct wl_array *ids) {
  (void)data; (void)wm; (void)ids;
}
static void wm_noop_str(void *data, struct org_kde_plasma_window_management *wm,
                        const char *s) {
  (void)data; (void)wm; (void)s;
}
static void wm_noop_win(void *data, struct org_kde_plasma_window_management *wm) {
  (void)data; (void)wm;
}

static const struct org_kde_plasma_window_management_listener wm_listener = {
  .show_desktop_changed = wm_noop_state,
  .window = wm_noop_id,
  .stacking_order_changed = wm_noop_array,
  .stacking_order_uuid_changed = wm_noop_str,
  .window_with_uuid = on_window_with_uuid,
  .stacking_order_changed_2 = wm_noop_win,
};

/* ------------------------------ registry ------------------------------ */

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *interface, uint32_t version) {
  PlasmaCtx *ctx = data;
  if (strcmp(interface, "org_kde_plasma_window_management") != 0) return;
  /* The global must be bound at version >= 17; clamp to what the server offers. */
  uint32_t bind_version = version < 17 ? version : 17;
  ctx->wm = wl_registry_bind(reg, name,
                             &org_kde_plasma_window_management_interface,
                             bind_version);
}

static void registry_global_remove(void *data, struct wl_registry *reg,
                                    uint32_t name) {
  (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener reg_listener = {
  .global = registry_global,
  .global_remove = registry_global_remove,
};

/* ------------------------------ public API ------------------------------ */

void cu_plasma_append_windows(CuJson *j, GHashTable *existing_pids) {
  if (!j || !cu_session_is_wayland()) return;

  PlasmaCtx ctx;
  ctx.display = NULL;
  ctx.registry = NULL;
  ctx.wm = NULL;
  ctx.windows = g_ptr_array_new_with_free_func((GDestroyNotify)pw_free);

  ctx.display = wl_display_connect(NULL);
  if (!ctx.display) goto done;

  ctx.registry = wl_display_get_registry(ctx.display);
  wl_registry_add_listener(ctx.registry, &reg_listener, &ctx);
  wl_display_roundtrip(ctx.display);

  if (!ctx.wm) goto done;

  org_kde_plasma_window_management_add_listener(ctx.wm, &wm_listener, &ctx);
  wl_display_roundtrip(ctx.display); /* window_with_uuid list arrives */
  wl_display_roundtrip(ctx.display);  /* per-window properties arrive */

  for (guint i = 0; i < ctx.windows->len; i++) {
    PlasmaWindow *pw = g_ptr_array_index(ctx.windows, i);
    if (pw->unmapped) continue;
    /* Skip a window the AT-SPI source already listed (matched by pid). */
    if (existing_pids && pw->pid > 0 &&
        g_hash_table_contains(existing_pids, GINT_TO_POINTER(pw->pid)))
      continue;

    char *key = g_strdup_printf("%d|%s|%s", pw->pid,
                                pw->app_id ? pw->app_id : "",
                                pw->title ? pw->title : "");
    long long wid = (long long)(g_str_hash(key) & 0x7fffffff);
    g_free(key);

    const char *app = pw->app_id && *pw->app_id ? pw->app_id
                      : (pw->title ? pw->title : "");
    bool minimized = (pw->state & CU_PW_STATE_MINIMIZED) != 0;

    cu_json_obj_begin(j);
    cu_json_key(j, "id"); cu_json_int(j, wid);
    cu_json_key(j, "app"); cu_json_str(j, app);
    cu_json_key(j, "title"); cu_json_str(j, pw->title ? pw->title : "");
    cu_json_key(j, "role"); cu_json_str(j, "frame");
    cu_json_key(j, "pid"); cu_json_int(j, pw->pid);
    cu_json_key(j, "onScreen");
    cu_json_bool(j, pw->has_geometry && !minimized);
    cu_json_key_obj(j, "bounds");
    cu_json_key(j, "x");
    cu_json_int(j, pw->has_geometry ? pw->x : CU_INVALID_COORD);
    cu_json_key(j, "y");
    cu_json_int(j, pw->has_geometry ? pw->y : CU_INVALID_COORD);
    cu_json_key(j, "width"); cu_json_int(j, pw->width);
    cu_json_key(j, "height"); cu_json_int(j, pw->height);
    cu_json_obj_end(j);
    cu_json_obj_end(j);
  }

done:
  g_ptr_array_free(ctx.windows, TRUE);
  if (ctx.registry) wl_registry_destroy(ctx.registry);
  if (ctx.display) wl_display_disconnect(ctx.display);
}

#else  /* !CU_HAVE_WAYLAND_CLIENT */

void cu_plasma_append_windows(CuJson *j, GHashTable *existing_pids) {
  (void)j;
  (void)existing_pids;
}

#endif /* CU_HAVE_WAYLAND_CLIENT */

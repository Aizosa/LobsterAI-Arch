#include "cu.h"

#include <json-glib/json-glib.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

/*
 * Entry point: a line-delimited JSON-RPC 2.0 server on stdin/stdout.
 *
 * Design notes
 *  - One JSON object per line. Requests are handled serially: accessibility
 *    calls are synchronous D-Bus round trips and interleaving them would only
 *    add races around the state_id -> node cache.
 *  - Any failure is reported as a JSON-RPC error object, never as a crash, so
 *    the MCP layer can surface a precise reason to the model.
 *  - stdout is protocol-only; all diagnostics go to stderr.
 */

volatile bool g_cu_user_cancelled = false;

static CuA11y *g_a11y = NULL;

/* ------------------------------------------------------------ JSON access */

static const char *param_str(JsonObject *o, const char *key) {
  if (!o || !json_object_has_member(o, key)) return NULL;
  JsonNode *n = json_object_get_member(o, key);
  if (!n || json_node_get_value_type(n) != G_TYPE_STRING) return NULL;
  return json_node_get_string(n);
}

static long long param_int(JsonObject *o, const char *key, long long fallback) {
  if (!o || !json_object_has_member(o, key)) return fallback;
  JsonNode *n = json_object_get_member(o, key);
  if (!n || json_node_get_value_type(n) != G_TYPE_INT64) {
    if (n && json_node_get_value_type(n) == G_TYPE_DOUBLE) {
      return (long long)json_node_get_double(n);
    }
    return fallback;
  }
  return json_node_get_int(n);
}

static double param_double(JsonObject *o, const char *key, double fallback) {
  if (!o || !json_object_has_member(o, key)) return fallback;
  JsonNode *n = json_object_get_member(o, key);
  GType t = json_node_get_value_type(n);
  if (t == G_TYPE_DOUBLE) return json_node_get_double(n);
  if (t == G_TYPE_INT64) return (double)json_node_get_int(n);
  return fallback;
}

static bool param_bool(JsonObject *o, const char *key, bool fallback) {
  if (!o || !json_object_has_member(o, key)) return fallback;
  JsonNode *n = json_object_get_member(o, key);
  if (json_node_get_value_type(n) != G_TYPE_BOOLEAN) return fallback;
  return json_node_get_boolean(n);
}

/* unsigned long, for X11 window ids. JSON has no unsigned type, so the value
   arrives as an int64 and is range-checked before narrowing: a window id is a
   full 32-bit XID and must not be sign-extended into a 64-bit value. */
static unsigned long param_ulong(JsonObject *o, const char *key,
                                 unsigned long fallback) {
  if (!o || !json_object_has_member(o, key)) return fallback;
  JsonNode *n = json_object_get_member(o, key);
  if (!n) return fallback;
  GType t = json_node_get_value_type(n);
  long long v;
  if (t == G_TYPE_INT64) v = json_node_get_int(n);
  else if (t == G_TYPE_DOUBLE) v = (long long)json_node_get_double(n);
  else return fallback;
  if (v <= 0) return fallback;
  return (unsigned long)v;
}

/* The window argument is an object, not a scalar. */
static JsonObject *param_obj(JsonObject *o, const char *key) {
  if (!o || !json_object_has_member(o, key)) return NULL;
  JsonNode *n = json_object_get_member(o, key);
  if (!JSON_NODE_HOLDS_OBJECT(n)) return NULL;
  return json_node_get_object(n);
}

/* --------------------------------------------------------------- responses */

static void emit_raw(const char *json) {
  fputs(json, stdout);
  fputc('\n', stdout);
  fflush(stdout);
}

static void emit_result(long long id, const char *payload) {
  CuJson j; cu_json_init(&j);
  cu_json_obj_begin(&j);
  cu_json_key(&j, "jsonrpc"); cu_json_str(&j, "2.0");
  cu_json_key(&j, "id"); cu_json_int(&j, id);
  /* payload is already a serialized JSON value, so it is appended verbatim
     rather than routed through the writer. */
  cu_json_key(&j, "result");
  g_string_append(j.buf, payload ? payload : "null");
  cu_json_obj_end(&j);
  emit_raw(j.buf->str);
  cu_json_free(&j);
}

static void emit_result_string(long long id, const char *text) {
  CuJson inner; cu_json_init(&inner);
  cu_json_obj_begin(&inner);
  cu_json_key(&inner, "content");
  cu_json_arr_begin(&inner);
  cu_json_obj_begin(&inner);
  cu_json_key(&inner, "type"); cu_json_str(&inner, "text");
  cu_json_key(&inner, "text"); cu_json_str(&inner, text ? text : "");
  cu_json_obj_end(&inner);
  cu_json_arr_end(&inner);
  cu_json_obj_end(&inner);
  emit_result(id, inner.buf->str);
  cu_json_free(&inner);
}

/*
 * A specific reason beats a generic one. On Wayland the backend records why the
 * portal refused or timed out, and that message is what lets the model tell the
 * user "click Allow on the dialog" instead of "input failed".
 */
static const char *input_error_message(void) {
  const char *why = cu_input_last_error();
  if (why && *why) return why;
  return "Input injection is unavailable on this session.";
}

static void emit_error(long long id, int code, const char *message) {
  CuJson j; cu_json_init(&j);
  cu_json_obj_begin(&j);
  cu_json_key(&j, "jsonrpc"); cu_json_str(&j, "2.0");
  cu_json_key(&j, "id"); cu_json_int(&j, id);
  cu_json_key_obj(&j, "error");
  cu_json_key(&j, "code"); cu_json_int(&j, code);
  cu_json_key(&j, "message"); cu_json_str(&j, message);
  cu_json_obj_end(&j);
  cu_json_obj_end(&j);
  emit_raw(j.buf->str);
  cu_json_free(&j);
}

/* ---------------------------------------------------------------- dispatch */

/*
 * Resolves the X11 window id for a request that only carries an AT-SPI window.
 *
 * Callers pass `window: { app, id, title, pid }`; the id is AT-SPI's and means
 * nothing to the X server. EWMH needs the real X id, so we look it up from the
 * pid and title the caller already has. A zero return is not an error — it just
 * means this window has no X11 route (native Wayland client, or a toolkit that
 * publishes neither pid nor a matching title), and the caller falls back to
 * AT-SPI.
 */
static unsigned long resolve_x11_window(JsonObject *params) {
  JsonObject *window = param_obj(params, "window");
  JsonObject *src = window ? window : params;
  int pid = (int)param_int(src, "pid", 0);
  const char *title = param_str(src, "title");
  const char *app = param_str(src, "app");
  if (pid <= 0 && !title && !app) return 0;
  return cu_x11_find_window(pid, title, app);
}

/* Explicit handle wins; otherwise resolve from the window object. The resolved
   handle both targets EWMH requests and gates the XTest fallback on Wayland. */
static unsigned long window_x11_handle(JsonObject *params) {
  unsigned long xw = param_ulong(params, "x11_window", 0);
  if (xw != 0) return xw;
  return resolve_x11_window(params);
}

static long long window_id_from(JsonObject *params) {
  JsonObject *win = param_obj(params, "window");
  if (win) {
    long long id = param_int(win, "id", -1);
    if (id >= 0) return id;
  }
  return param_int(params, "window_id", -1);
}

static const char *window_app_from(JsonObject *params) {
  JsonObject *win = param_obj(params, "window");
  return win ? param_str(win, "app") : NULL;
}

/*
 * Screen coordinates for a pixel click. The MCP layer passes pixels of the
 * screenshot it gave the model; since capture is 1:1 with the screen on Linux
 * (no HiDPI downscaling on X11 unless the client scales), pixels are screen
 * coordinates with the window origin added back.
 */
static bool resolve_click_point(JsonObject *params, double *out_x, double *out_y) {
  if (!params) return false;
  double x = param_double(params, "x", CU_INVALID_COORD);
  double y = param_double(params, "y", CU_INVALID_COORD);
  if (x == CU_INVALID_COORD || y == CU_INVALID_COORD) return false;
  *out_x = x;
  *out_y = y;
  return true;
}

static void handle_request(JsonObject *req) {
  long long id = param_int(req, "id", 0);
  const char *method = param_str(req, "method");
  JsonObject *params = param_obj(req, "params");

  if (!method) {
    emit_error(id, -32600, "Invalid Request: missing method");
    return;
  }

  if (strcmp(method, "shutdown") == 0) {
    emit_result_string(id, "ok");
    g_main_loop_quit(NULL);
    exit(0);
  }

  if (strcmp(method, "check_permissions") == 0) {
    char *status = cu_permissions_status();
    emit_result_string(id, status);
    g_free(status);
    return;
  }

  if (strcmp(method, "request_permissions") == 0) {
    char *status = cu_permissions_status();
    emit_result_string(id, status);
    g_free(status);
    return;
  }

  /*
   * The environment report. Everything the caller could otherwise only discover
   * by trial and error: which desktop we think this is, which session protocol,
   * which portal interfaces exist and at what version, which private suite APIs
   * are reachable, and which backend each verb will therefore take.
   *
   * This is deliberately separate from check_permissions. check_permissions
   * answers "may I do this", which on Linux mostly has the answer "yes,
   * eventually, after a dialog"; this answers "what will actually happen",
   * which is what the model needs to plan a task instead of discovering the
   * layout by failing.
   */
  if (strcmp(method, "describe_environment") == 0) {
    char *report = cu_desktop_report_json();
    emit_result_string(id, report);
    g_free(report);
    return;
  }

  /*
   * GNOME's accessibility switch. Reading it is part of the environment report;
   * writing it is offered as an explicit verb because it changes the user's
   * desktop settings and only takes effect for applications started afterwards.
   */
  if (strcmp(method, "set_accessibility") == 0) {
    bool enabled = param_bool(params, "enabled", true);
    if (!cu_gnome_set_toolkit_accessibility(enabled)) {
      emit_error(id, -32000,
                 "Could not change org.gnome.desktop.interface "
                 "toolkit-accessibility. Is gsettings installed?");
      return;
    }
    CuJson j; cu_json_init(&j);
    cu_json_obj_begin(&j);
    cu_json_key(&j, "ok"); cu_json_bool(&j, true);
    cu_json_key(&j, "toolkitAccessibility"); cu_json_bool(&j, enabled);
    cu_json_key(&j, "note");
    cu_json_str(&j,
        "The setting is changed, but applications read it once at startup. "
        "Restart the app for it to appear in the accessibility tree.");
    cu_json_obj_end(&j);
    emit_result_string(id, j.buf->str);
    cu_json_free(&j);
    return;
  }

  /*
   * Opens the Wayland consent dialog up front.
   *
   * Without this, the first click or keystroke blocks for as long as it takes
   * the user to read the dialog — which looks like a hang from the caller's
   * side and can trip the 30s request timeout. Letting the model request
   * permission as its own step means the wait happens where it is expected.
   */
  if (strcmp(method, "request_input_access") == 0) {
    if (!cu_session_is_wayland()) {
      emit_result_string(id, "Not needed: this is an X11 session, where XTest "
                             "injects input without a permission prompt.");
      return;
    }
    if (cu_input_open_wayland_session()) {
      emit_result_string(id, "Input permission granted; RemoteDesktop session active.");
      return;
    }
    const char *why = cu_input_last_error();
    char *msg = g_strdup_printf("Input permission was not granted. %s",
                                why ? why : "");
    emit_error(id, -32000, msg);
    g_free(msg);
    return;
  }

  if (strcmp(method, "list_windows") == 0) {
    char *payload = cu_a11y_list_windows(g_a11y);
    if (!payload) { emit_error(id, -32000, "Unable to read the accessibility tree"); return; }
    emit_result_string(id, payload);
    g_free(payload);
    return;
  }

  if (strcmp(method, "list_apps") == 0) {
    bool include_installed = param_bool(params, "include_installed", true);
    char *payload = cu_a11y_list_apps(g_a11y, include_installed);
    if (!payload) { emit_error(id, -32000, "Unable to read the accessibility tree"); return; }
    emit_result_string(id, payload);
    g_free(payload);
    return;
  }

  if (strcmp(method, "get_window") == 0) {
    long long wid = window_id_from(params);
    char *app = NULL, *title = NULL;
    int pid = 0;
    if (!cu_a11y_find_window(g_a11y, wid, &app, &title, &pid)) {
      emit_error(id, -32000, "Window not found; call list_windows again");
      return;
    }
    CuJson j; cu_json_init(&j);
    cu_json_obj_begin(&j);
    cu_json_key(&j, "id"); cu_json_int(&j, wid);
    cu_json_key(&j, "app"); cu_json_str(&j, app);
    cu_json_key(&j, "title"); cu_json_str(&j, title);
    cu_json_key(&j, "pid"); cu_json_int(&j, pid);
    unsigned long xw = cu_x11_find_window(pid, title, app);
    if (xw != 0) {
      cu_json_key(&j, "x11_window"); cu_json_int(&j, (long long)xw);
    }
    cu_json_obj_end(&j);
    emit_result_string(id, j.buf->str);
    cu_json_free(&j);
    g_free(app); g_free(title);
    return;
  }

  if (strcmp(method, "get_window_state") == 0) {
    long long wid = window_id_from(params);
    const char *app = window_app_from(params);
    bool shot = param_bool(params, "include_screenshot", true);
    bool text = param_bool(params, "include_text", true);
    int max_nodes = (int)param_int(params, "max_nodes", CU_MAX_NODES_DEFAULT);
    int text_limit = (int)param_int(params, "text_limit", CU_MAX_TEXT_DEFAULT);

    char *payload = cu_a11y_window_state(g_a11y, wid, app, shot, text,
                                         max_nodes, text_limit);
    if (!payload) { emit_error(id, -32000, "Window not found; call list_windows again"); return; }
    emit_result_string(id, payload);
    g_free(payload);
    return;
  }

  if (strcmp(method, "activate_window") == 0) {
    long long wid = window_id_from(params);

    /*
     * On X11 the window manager is the authority and EWMH is the request, so
     * that path is both more likely to work and better behaved (it carries a
     * source indication, so the WM can apply its focus-stealing policy) than
     * asking the toolkit directly.
     *
     * The caller normally only knows the AT-SPI window id, so when no explicit
     * x11_window arrives we resolve one from the pid and title the MCP layer
     * echoes back. Both id spaces are opaque to the caller; that is why the
     * lookup lives here and not in the bridge.
     */
    unsigned long x11_window = param_ulong(params, "x11_window", 0);
    if (x11_window == 0) {
      x11_window = resolve_x11_window(params);
    }
    if (x11_window != 0 && cu_x11_activate(x11_window)) {
      emit_result_string(id, "Window activated through the window manager.");
      return;
    }

    char *r = cu_a11y_activate_window(g_a11y, wid);
    if (!r) { emit_error(id, -32000, "AT-SPI could not raise this window"); return; }
    emit_result_string(id, r);
    g_free(r);
    return;
  }

  if (strcmp(method, "close") == 0) {
    long long wid = window_id_from(params);
    char *r = cu_a11y_close_window(g_a11y, wid);
    if (r) {
      emit_result_string(id, r);
      g_free(r);
      return;
    }
    /*
     * Fall back to the window manager. _NET_CLOSE_WINDOW is the same request a
     * titlebar click makes, so the app still gets a chance to prompt about
     * unsaved work. Only available on X11; on Wayland there is no cross-client
     * close and the AT-SPI path is the only one.
     *
     * Resolve by pid+title rather than guessing at _NET_ACTIVE_WINDOW: closing
     * whatever happens to be focused would be a spectacular way to lose someone
     * else's unsaved document.
     */
    unsigned long x11_window = param_ulong(params, "x11_window", 0);
    if (x11_window == 0) x11_window = resolve_x11_window(params);
    if (x11_window != 0 && cu_x11_close(x11_window)) {
      emit_result_string(id, "Close requested through the window manager.");
      return;
    }
    emit_error(id, -32000, "Window not found, or it exposes no close action");
    return;
  }

  if (strcmp(method, "launch_app") == 0) {
    /* Accept whichever field the client filled in; the MCP layer sends one of
       these depending on how the model addressed the app. */
    const char *target = param_str(params, "bundle_id");
    if (!target) target = param_str(params, "bundleId");
    if (!target) target = param_str(params, "app");
    if (!target) target = param_str(params, "name");
    if (!target) target = param_str(params, "path");
    if (!target) {
      emit_error(id, -32602, "launch_app needs bundle_id, app, name, or path");
      return;
    }
    char *resolved = NULL;
    char *r = cu_desktop_launch(target, &resolved);
    if (!r) {
      char *msg = g_strdup_printf(
          "No installed application matches \"%s\". Call list_apps and use the "
          "bundleId it reports, or pass an absolute executable path.", target);
      emit_error(id, -32000, msg);
      g_free(msg);
      return;
    }
    emit_result_string(id, r);
    g_free(r); g_free(resolved);
    return;
  }

  if (strcmp(method, "resolve_app") == 0) {
    const char *target = param_str(params, "bundle_id");
    if (!target) target = param_str(params, "app");
    if (!target) target = param_str(params, "name");
    if (!target) { emit_error(id, -32602, "resolve_app needs an app identifier"); return; }
    /* Dry run: report what the identifier maps to without starting anything. */
    char *exec_line = cu_desktop_resolve(target);
    CuJson j; cu_json_init(&j);
    cu_json_obj_begin(&j);
    cu_json_key(&j, "resolved"); cu_json_bool(&j, exec_line != NULL);
    cu_json_key(&j, "query"); cu_json_str(&j, target);
    if (exec_line) { cu_json_key(&j, "exec"); cu_json_str(&j, exec_line); }
    cu_json_obj_end(&j);
    emit_result_string(id, j.buf->str);
    cu_json_free(&j);
    g_free(exec_line);
    return;
  }

  if (strcmp(method, "click") == 0) {
    int element_index = (int)param_int(params, "element_index", -1);
    const char *state_id = param_str(params, "state_id");

    if (element_index >= 0 && state_id) {
      char *r = cu_a11y_click_element(g_a11y, state_id, element_index);
      if (r) { emit_result_string(id, r); g_free(r); return; }
      /* Fall through to a pixel click if the element is no longer exposed. */
    }

    double x, y;
    if (!resolve_click_point(params, &x, &y)) {
      emit_error(id, -32602, "click needs element_index + state_id, or x and y");
      return;
    }
    int button = 1;
    const char *mb = param_str(params, "mouse_button");
    if (mb) {
      if (mb[0] == 'r') button = 3;
      else if (mb[0] == 'm') button = 2;
    }
    int count = (int)param_int(params, "click_count", 1);
    if (!cu_input_click_at(x, y, button, count, window_x11_handle(params))) {
      emit_error(id, -32000, input_error_message());
      return;
    }
    emit_result_string(id, "Click completed.");
    return;
  }

  if (strcmp(method, "press_key") == 0) {
    const char *key = param_str(params, "key");
    if (!key) { emit_error(id, -32602, "press_key needs key"); return; }
    int repeat = (int)param_int(params, "repeat", 1);
    if (!cu_input_press_key(key, repeat, window_x11_handle(params))) {
      emit_error(id, -32000, input_error_message());
      return;
    }
    emit_result_string(id, "Key press completed.");
    return;
  }

  if (strcmp(method, "type_text") == 0) {
    const char *text = param_str(params, "text");
    if (!text) { emit_error(id, -32602, "type_text needs text"); return; }
    if (!cu_input_type_text(text, window_x11_handle(params))) {
      emit_error(id, -32000, input_error_message());
      return;
    }
    emit_result_string(id, "Text typed.");
    return;
  }

  if (strcmp(method, "scroll") == 0) {
    double x, y;
    if (!resolve_click_point(params, &x, &y)) {
      emit_error(id, -32602, "scroll needs x and y on Linux");
      return;
    }
    int sx = (int)param_double(params, "scrollX", 0);
    int sy = (int)param_double(params, "scrollY", 0);
    if (!cu_input_scroll_at(x, y, sx, sy, window_x11_handle(params))) {
      emit_error(id, -32000, input_error_message());
      return;
    }
    emit_result_string(id, "Scroll completed.");
    return;
  }

  if (strcmp(method, "drag") == 0) {
    double fx = param_double(params, "from_x", CU_INVALID_COORD);
    double fy = param_double(params, "from_y", CU_INVALID_COORD);
    double tx = param_double(params, "to_x", CU_INVALID_COORD);
    double ty = param_double(params, "to_y", CU_INVALID_COORD);
    if (fx == CU_INVALID_COORD || tx == CU_INVALID_COORD) {
      emit_error(id, -32602, "drag needs from_x/from_y and to_x/to_y");
      return;
    }
    if (!cu_input_drag(fx, fy, tx, ty, window_x11_handle(params))) {
      emit_error(id, -32000, input_error_message());
      return;
    }
    emit_result_string(id, "Drag completed.");
    return;
  }

  if (strcmp(method, "set_value") == 0) {
    const char *state_id = param_str(params, "state_id");
    const char *value = param_str(params, "value");
    int element_index = (int)param_int(params, "element_index", -1);
    char *r = cu_a11y_set_value(g_a11y, state_id, element_index, value);
    if (!r) { emit_error(id, -32000, "Element is not editable through the accessibility tree"); return; }
    emit_result_string(id, r);
    g_free(r);
    return;
  }

  if (strcmp(method, "perform_secondary_action") == 0) {
    const char *state_id = param_str(params, "state_id");
    const char *action = param_str(params, "action");
    int element_index = (int)param_int(params, "element_index", -1);
    char *r = cu_a11y_perform_action(g_a11y, state_id, element_index, action);
    if (!r) { emit_error(id, -32000, "Element does not expose that action"); return; }
    emit_result_string(id, r);
    g_free(r);
    return;
  }

  emit_error(id, -32601, "Method not found");
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv) {
  /* stdout is the protocol channel; keep it unbuffered and clean. */
  setvbuf(stdout, NULL, _IOLBF, 0);

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--version") == 0) {
      fprintf(stdout, "{\"version\":\"1.0.0\",\"platform\":\"linux\"}\n");
      return 0;
    }
    if (strcmp(argv[i], "--self-test") == 0) {
      CuJson j; cu_json_init(&j);
      cu_json_obj_begin(&j);
      cu_json_key(&j, "ok"); cu_json_bool(&j, true);
      cu_json_key(&j, "helper"); cu_json_str(&j, "lobsterai-computer-use-linux");
      cu_json_obj_end(&j);
      fprintf(stdout, "%s\n", j.buf->str);
      cu_json_free(&j);
      return 0;
    }
  }

  g_a11y = cu_a11y_new();
  if (!g_a11y) {
    fprintf(stderr, "[computer-use-linux] AT-SPI2 is unavailable; "
                    "install at-spi2-core and make sure the accessibility bus runs.\n");
  }
  cu_input_init();

  char *line = NULL;
  size_t cap = 0;
  ssize_t len;

  while ((len = getline(&line, &cap, stdin)) > 0) {
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[--len] = '\0';
    }
    if (len == 0) continue;

    GError *err = NULL;
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, line, len, &err)) {
      emit_error(0, -32700, err ? err->message : "Parse error");
      if (err) g_error_free(err);
      g_object_unref(parser);
      continue;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
      emit_error(0, -32600, "Invalid Request: expected a JSON object");
      g_object_unref(parser);
      continue;
    }

    /* json_parser owns the root; run the handler before unref. */
    handle_request(json_node_get_object(root));
    g_object_unref(parser);
  }

  free(line);
  cu_a11y_free(g_a11y);
  return 0;
}

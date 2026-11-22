/*
 * LobsterAI Computer Use — Linux helper.
 *
 * Speaks JSON-RPC 2.0 over stdin/stdout (one JSON object per line, no
 * Content-Length framing: same shape as the macOS helper's stdio transport).
 * One request per line in, one response per line out.
 *
 * Methods mirror the platform client module contract consumed by
 * src/main/computerUse/computerUseMcpServer.ts:
 *
 *   list_windows              list_apps               get_window
 *   get_window_state          activate_window         click
 *   press_key                 type_text               scroll
 *   drag                      set_value               perform_secondary_action
 *   check_permissions         request_permissions     shutdown
 *
 * Backends, selected at runtime rather than compile time so a single binary
 * works on both session types:
 *   - Accessibility tree : AT-SPI2 (D-Bus) — both X11 and Wayland.
 *   - Screenshots        : PipeWire portal (Wayland) or XGetImage (X11).
 *   - Input injection    : libei (Wayland) or XTest (X11).
 */
#ifndef LOBSTER_CU_H
#define LOBSTER_CU_H

#include <glib.h>
#include <stdbool.h>

#define CU_MAX_NODES_DEFAULT 350
#define CU_MAX_TEXT_DEFAULT 6000
#define CU_INVALID_COORD (-99999)
#define CU_MAX_KEYSTROKE 32

/* ---------------- JSON helpers ---------------- */

/* Minimal JSON writer over a GString; keeps us free of a JSON dependency. */
typedef struct {
  GString *buf;
  int depth;
  bool first[64];
} CuJson;

void cu_json_init(CuJson *j);
void cu_json_free(CuJson *j);
void cu_json_obj_begin(CuJson *j);
void cu_json_obj_end(CuJson *j);
void cu_json_arr_begin(CuJson *j);
void cu_json_arr_end(CuJson *j);
void cu_json_key(CuJson *j, const char *key);
void cu_json_str(CuJson *j, const char *s);
void cu_json_int(CuJson *j, long long v);
void cu_json_double(CuJson *j, double v);
void cu_json_bool(CuJson *j, bool v);
void cu_json_null(CuJson *j);
/* Attach a key and open an object/array in one call. */
void cu_json_key_obj(CuJson *j, const char *key);
void cu_json_key_arr(CuJson *j, const char *key);

/* ---------------- Request plumbing ---------------- */

typedef struct {
  GHashTable *params;   /* owned; values are GVariant */
  char *method;
  long long id;
  bool has_id;
} CuRequest;

typedef struct {
  bool ok;
  char *payload;        /* pre-serialized JSON value */
  char *error;          /* human readable message when !ok */
} CuResult;

/* ---------------- Accessibility backend ---------------- */

typedef struct _CuA11y CuA11y;

/* Lazily initialised; returns NULL when AT-SPI is unavailable. */
CuA11y *cu_a11y_new(void);
void cu_a11y_free(CuA11y *a);
bool cu_a11y_ready(CuA11y *a);
/* Whether this process attached to the accessibility bus (atspi_init succeeded).
   Different from cu_atspi_bus_reachable(), which only asks whether it is up. */
bool cu_atspi_attached(void);

/* Builds the list_windows payload. */
char *cu_a11y_list_windows(CuA11y *a);
/* Builds the list_apps payload. */
char *cu_a11y_list_apps(CuA11y *a, bool include_installed);

/* Resolves window -> accessible handle and caches it for the given id. */
bool cu_a11y_find_window(CuA11y *a, long long window_id, char **app_name,
                         char **title, int *pid);

/* Builds get_window_state: screenshot (base64) + accessibility outline. */
char *cu_a11y_window_state(CuA11y *a, long long window_id, const char *app,
                           bool want_screenshot, bool want_text, int max_nodes,
                           int text_limit);

/* Element actions reached through the accessibility tree. */
char *cu_a11y_click_element(CuA11y *a, const char *state_id, int element_index);
char *cu_a11y_set_value(CuA11y *a, const char *state_id, int element_index,
                        const char *value);
char *cu_a11y_perform_action(CuA11y *a, const char *state_id, int element_index,
                             const char *action);
char *cu_a11y_activate_window(CuA11y *a, long long window_id);
/* Asks the window to close; the app still decides (and may prompt to save). */
char *cu_a11y_close_window(CuA11y *a, long long window_id);

/* ---------------- Screen capture backend ---------------- */

typedef struct {
  unsigned char *pixels;   /* RGB, 3 bytes per pixel */
  int width;
  int height;
  int stride;
} CuImage;

/* Captures one window; NULL when the backend cannot (e.g. no portal grant).
   On a composited session the window's own framebuffer is unreadable, so the
   caller must supply the rect AT-SPI reported for it and the full-screen
   capture is cropped down to that rect. Passing w/h <= 0 returns the whole
   screen.

   x11_window is the X11 Window id when the caller happens to know it; 0 means
   "unknown", which is the normal case on Wayland where no X id exists for a
   native client. It is never an AT-SPI window id — those are hashes, and using
   one here would be a BadWindow. */
CuImage *cu_capture_window_rect(unsigned long x11_window, int x, int y, int w, int h);
CuImage *cu_capture_window(int x, int y, int w, int h);
/* True when the screenshot portal answers (i.e. capture can work at all). */
bool cu_capture_available(void);
/* Version of one xdg-desktop-portal interface, or 0 when it is not exported. */
int cu_portal_version(const char *interface);
/* Which capture backend this session will actually use, for reporting. */
const char *cu_capture_preferred_backend(void);
/* Why the last capture failed, or NULL. Cleared on each capture attempt. */
const char *cu_capture_last_error(void);
void cu_image_free(CuImage *img);
/* Base64-encodes as PNG; caller frees. */
char *cu_image_to_png_base64(const CuImage *img);
/* Encodes to PNG bytes; caller frees with g_free. */
unsigned char *cu_image_to_png(const CuImage *img, gsize *out_len);

/* ---------------- Input backend ---------------- */

void cu_input_init(void);
/* Moves the real pointer, clicks, and returns to where it was.
   `x11_window` is a resolved X11 handle when known; on a Wayland session it
   gates the XTest fallback to XWayland clients (0 = native Wayland, no fallback). */
bool cu_input_click_at(double x, double y, int button, int click_count,
                       unsigned long x11_window);
bool cu_input_drag(double from_x, double from_y, double to_x, double to_y,
                   unsigned long x11_window);
bool cu_input_scroll_at(double x, double y, int scroll_x, int scroll_y,
                        unsigned long x11_window);
/* key is "ctrl+shift+t" style. */
bool cu_input_press_key(const char *key, int repeat, unsigned long x11_window);
bool cu_input_type_text(const char *text, unsigned long x11_window);
/* Which injection backend this session will actually use, for reporting. */
const char *cu_input_preferred_backend(void);
/* Why the last injection failed, or NULL. */
const char *cu_input_last_error(void);
/* True when an X display is reachable (X11, or XWayland). */
bool cu_x11_available(void);
/*
 * Opens a RemoteDesktop-portal session, which is the only sanctioned way to
 * synthesize input on Wayland. Shows the compositor's consent dialog the first
 * time; the grant is then reused for the rest of the process.
 */
bool cu_input_open_wayland_session(void);
bool cu_remote_desktop_active(void);
/* Which device classes the active RemoteDesktop session actually granted. */
bool cu_rd_has_pointer(void);
bool cu_rd_has_keyboard(void);

/* ---- RemoteDesktop primitives, used by the Wayland input backend ---- */

/* Absolute pointer motion in logical (compositor) coordinates. */
bool cu_rd_pointer_motion_absolute(double x, double y);
/* button uses X11 numbering (1 left, 2 middle, 3 right); mapped to evdev. */
bool cu_rd_pointer_button(int x11_button, bool pressed);
bool cu_rd_pointer_axis_discrete(bool horizontal, int steps);
bool cu_rd_key_keysym(unsigned long keysym, bool pressed);
/* Sends a UTF-8 string through the EIS virtual text device when one is bound.
   Returns false when the session has no text capability; the caller then falls
   back to per-key injection. CJK and other non-ASCII scripts are not committed
   by the XWayland/XIM path even when the call succeeds. */
bool cu_rd_type_utf8(const char *utf8);

/* Maps an MCP-layer key name ("return", "f5", "a") to an X keysym. Works
   without an X display: it is a table lookup, not a server round trip. */
unsigned long cu_keysym_from_name(const char *name);

/* ---- Wayland platform probes ---- */

/* True when the compositor is wlroots-based, which changes what is possible. */
bool cu_wlroots_likely(void);
/* Probes ScreenCast without prompting; caches the answer. */
bool cu_wayland_probe_screencast(void);

/* ---- GNOME ---- */

typedef enum {
  CU_GNOME_EVAL_ABSENT = 0,   /* no GNOME Shell on this bus */
  CU_GNOME_EVAL_X11_ONLY,     /* shell present, method not exported (Wayland) */
  CU_GNOME_EVAL_OK,           /* reachable and working */
} CuGnomeEvalState;

CuGnomeEvalState cu_gnome_eval_state(void);
const char *cu_gnome_eval_state_id(CuGnomeEvalState s);
/* Runs a snippet in the shell; NULL when unavailable or it failed. */
char *cu_gnome_eval(const char *script);
/* Raises a GNOME window by title substring. NULL when it could not be done. */
char *cu_gnome_activate_by_title(const char *title_hint);
/* Silent capture via org.gnome.Shell.Screenshot. Returns an owned path or NULL. */
bool cu_gnome_screenshot_path_available(void);
char *cu_gnome_screenshot_to_file(int x, int y, int w, int h, bool include_cursor);
bool cu_gnome_set_toolkit_accessibility(bool enabled);
char *cu_gnome_shell_version(void);

/* ---- XFCE ---- */

bool cu_xfce_session_present(void);
/* Full-screen capture via xfce4-screenshooter; NULL when unavailable. */
char *cu_xfce_screenshot_to_file(int x, int y, int w, int h);
/* Capability note for the caller, or NULL when not an XFCE session. */
char *cu_xfce_capability_note(void);

/* ---- Accessibility bus reachability ---- */

bool cu_atspi_bus_reachable(void);

/* ---------------- Desktop environment ---------------- */

/*
 * Linux is not one platform: what works depends on the desktop (each provides
 * its own portal backend and its own private escape hatches) and on the session
 * protocol (X11 lets us read pixels and inject input; Wayland forbids both).
 * Everything that varies goes through the struct below rather than scattered
 * getenv() calls, so the report and the backends can never disagree.
 */
typedef enum {
  CU_DE_UNKNOWN = 0,
  CU_DE_KDE,
  CU_DE_GNOME,
  CU_DE_XFCE,
  CU_DE_MATE,
  CU_DE_CINNAMON,
  CU_DE_LXQT,
  CU_DE_WLROOTS,   /* sway / Hyprland / river / Wayfire: wlroots protocols */
} CuDesktopKind;

typedef struct {
  CuDesktopKind kind;
  const char *name;         /* static, human readable */
  const char *session_type; /* "wayland" | "x11" | "unknown" */
  bool is_wayland;
  bool is_x11;
  bool has_x11;             /* DISPLAY set — true under XWayland too */

  bool has_portal;
  int portal_screenshot_version;      /* 0 = interface absent */
  int portal_screencast_version;
  int portal_remote_desktop_version;
  int portal_settings_version;

  bool gnome_shell;          /* org.gnome.Shell name owned */
  bool gnome_shell_screenshot;
  bool toolkit_accessibility;

  bool cmd_spectacle;
  bool cmd_grim;
  bool cmd_gnome_screenshot;
  bool cmd_xfce4_screenshooter;

  bool x11_ewmh;
  bool x11_has_active_window;
  bool x11_has_close_window;
  char *x11_wm_name;          /* owned */
} CuDesktop;

void cu_desktop_detect(CuDesktop *out);
const char *cu_desktop_kind_id(CuDesktopKind kind);
/* Full capability report, for the `describe_environment` method. */
char *cu_desktop_report_json(void);

const char *cu_session_type(void);
bool cu_session_is_wayland(void);
bool cu_have_command(const char *name);
bool cu_bus_name_has_owner(const char *name);
/* Reads a boolean through org.freedesktop.portal.Settings; returns fallback
   when the portal or the schema is unavailable. */
bool cu_gsettings_bool(const char *schema, const char *key, bool fallback);

/* ---------------- Windows / window manager ---------------- */

/* Fills the x11_* fields of `out`. No-op without X11. */
void cu_x11_detect_wm(CuDesktop *out);
/* Asks the window manager to close an X11 window (_NET_CLOSE_WINDOW). */
bool cu_x11_close(unsigned long x11_window);
/* The currently focused X11 window id, or 0. */
unsigned long cu_x11_active_window(void);
/*
 * Maps an AT-SPI window to its X11 window id by pid and title.
 *
 * The MCP layer passes an AT-SPI window id and nothing else, so without this
 * every EWMH request would go out with x11_window = 0 and fall through to the
 * toolkit path. Returns 0 when the X11 side cannot identify the window, which
 * callers must treat as "no X11 route available" rather than as failure.
 */
unsigned long cu_x11_find_window_by_pid_title(int pid, const char *title);
/*
 * Full form of the lookup: also matches WM_CLASS against the desktop entry for
 * `wm_class_query` (app id, display name, or executable) and ranks candidates
 * by pid, class and title. Prefer this; the pid-only form misses every XWayland
 * client whose _NET_WM_PID is absent.
 */
unsigned long cu_x11_find_window(int pid, const char *title, const char *wm_class_query);
/*
 * KDE plasma-window-management Wayland enumeration fallback. Appends window
 * entries to a JSON array already opened by the caller ("windows"). Windows
 * whose pid is already present in `existing_pids` (a GHashTable keyed by
 * GINT_TO_POINTER(pid)) are skipped so the two sources never duplicate a
 * window; the table may be NULL to append every reported window. A no-op stub
 * when wayland-client is absent or the compositor does not export the
 * protocol, so the AT-SPI list stays authoritative on every other desktop.
 */
void cu_plasma_append_windows(CuJson *j, GHashTable *existing_pids);

/* Fills `out` with lowercased WM_CLASS candidates derived from a desktop entry. */
void cu_desktop_wm_class_candidates(const char *query, GPtrArray *out);
/* Reads _NET_WM_PID and _NET_WM_NAME off an X window; each out param may be NULL. */
bool cu_x11_window_identity(unsigned long x11_window, int *pid, char **title);

/* ---------------- Misc ---------------- */

/* Permission report for check_permissions. Returns JSON. */
char *cu_permissions_status(void);

/*
 * Launches an application by desktop-entry app id, display name, or executable
 * path. Returns a status string, or NULL when nothing matched. The app is
 * detached into its own session so it outlives this helper.
 */
char *cu_desktop_launch(const char *query, char **resolved_name);
/* Pure resolution: the Exec line `query` maps to, or NULL. Starts nothing. */
char *cu_desktop_resolve(const char *query);

/*
 * Asks the window manager to raise an X11 window (EWMH _NET_ACTIVE_WINDOW).
 * Returns false when there is no X window to act on — which is the normal case
 * for a native Wayland client, where focus is the compositor's decision alone.
 */
bool cu_x11_activate(unsigned long x11_window);

/* Raised by the real-Escape watcher: aborts the current turn. */
extern volatile bool g_cu_user_cancelled;

#endif /* LOBSTER_CU_H */

#include "cu.h"

#include <fcntl.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Application discovery and launching.
 *
 * The MCP layer's launch_app accepts an app id, a name, or an executable path.
 * On Linux the natural identifier is the desktop file's basename without
 * ".desktop" (the "app id"), so that is what we advertise as `bundleId`. This
 * mirrors what the macOS helper reports for a bundle identifier.
 */

typedef struct {
  char *id;         /* file basename without .desktop */
  char *name;       /* Name= (localized variant when available) */
  char *exec;       /* Exec= with field codes stripped */
  char *icon;
  char *wm_class;   /* StartupWMClass=, the declared link to WM_CLASS */
} DesktopEntry;

static void entry_free(gpointer p) {
  DesktopEntry *e = p;
  if (!e) return;
  g_free(e->id);
  g_free(e->name);
  g_free(e->exec);
  g_free(e->icon);
  g_free(e->wm_class);
  g_free(e);
}

static char *strip_field_codes(const char *exec) {
  if (!exec) return NULL;
  GString *out = g_string_new(NULL);
  for (const char *p = exec; *p; ) {
    if (*p == '%' && p[1]) {
      /* %f %F %u %U %i %c %k %d %D %n %N %v %m: drop the token. */
      p += 2;
      continue;
    }
    g_string_append_c(out, *p);
    p++;
  }
  return g_strstrip(g_string_free(out, FALSE));
}

static DesktopEntry *parse_entry(const char *path) {
  GKeyFile *kf = g_key_file_new();
  if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
    g_key_file_free(kf);
    return NULL;
  }
  gchar *name = g_key_file_get_locale_string(kf, "Desktop Entry", "Name", NULL, NULL);
  gchar *exec = g_key_file_get_string(kf, "Desktop Entry", "Exec", NULL);
  gchar *icon = g_key_file_get_string(kf, "Desktop Entry", "Icon", NULL);
  gboolean hidden = g_key_file_get_boolean(kf, "Desktop Entry", "NoDisplay", NULL);
  gchar *type = g_key_file_get_string(kf, "Desktop Entry", "Type", NULL);
  gchar *wm_class = g_key_file_get_string(kf, "Desktop Entry", "StartupWMClass", NULL);

  DesktopEntry *e = NULL;
  if (!hidden && name && exec && (!type || strcmp(type, "Application") == 0)) {
    e = g_new0(DesktopEntry, 1);
    char *base = g_path_get_basename(path);
    size_t blen = strlen(base);
    /* ".desktop" is 8 characters; the check keeps at least one character of
       name so a bare ".desktop" file cannot produce an empty id. */
    if (blen > 8 && g_str_has_suffix(base, ".desktop")) base[blen - 8] = '\0';
    e->id = g_strdup(base);
    g_free(base);
    e->name = g_strdup(name);
    e->exec = strip_field_codes(exec);
    e->icon = icon ? g_strdup(icon) : NULL;
    e->wm_class = wm_class ? g_strdup(wm_class) : NULL;
  }

  g_free(name);
  g_free(exec);
  g_free(icon);
  g_free(type);
  g_free(wm_class);
  g_key_file_free(kf);
  return e;
}

static void scan_dir(GHashTable *seen, const char *dir) {
  GDir *d = g_dir_open(dir, 0, NULL);
  if (!d) return;
  const char *name;
  while ((name = g_dir_read_name(d))) {
    if (!g_str_has_suffix(name, ".desktop")) continue;
    char *full = g_build_filename(dir, name, NULL);
    char *base = g_path_get_basename(full);
    /* Later directories take precedence in XDG rules; first hit wins here. */
    if (!g_hash_table_contains(seen, base)) {
      DesktopEntry *e = parse_entry(full);
      if (e) {
        g_hash_table_insert(seen, g_strdup(base), e);
      }
    }
    g_free(base);
    g_free(full);
  }
  g_dir_close(d);
}

/* Defined in a11y.c's list_apps path; emits an array of installed apps. */
void cu_desktop_list_installed(CuJson *j) {
  GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, entry_free);

  const char *data_home = g_get_user_data_dir();
  const char *data_dirs = g_getenv("XDG_DATA_DIRS");
  if (!data_dirs || !*data_dirs) data_dirs = "/usr/local/share:/usr/share";

  /* User entries shadow system ones, so scan them first. */
  char *user_apps = g_build_filename(data_home, "applications", NULL);
  scan_dir(seen, user_apps);
  g_free(user_apps);

  gchar **dirs = g_strsplit(data_dirs, ":", -1);
  for (int i = 0; dirs[i]; i++) {
    char *apps = g_build_filename(dirs[i], "applications", NULL);
    scan_dir(seen, apps);
    g_free(apps);
  }
  g_strfreev(dirs);

  GHashTableIter it;
  gpointer k, v;
  /* Sort by name so repeated list_apps calls are stable and diffable. */
  GList *keys = g_list_alloc();
  keys = g_list_remove(keys, NULL);
  g_hash_table_iter_init(&it, seen);
  while (g_hash_table_iter_next(&it, &k, &v)) keys = g_list_prepend(keys, k);
  keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);
  for (GList *l = keys; l; l = l->next) {
    DesktopEntry *e = g_hash_table_lookup(seen, l->data);
    if (!e || !e->exec) continue;
    cu_json_obj_begin(j);
    cu_json_key(j, "app"); cu_json_str(j, e->name ? e->name : e->id);
    cu_json_key(j, "bundleId"); cu_json_str(j, e->id);
    cu_json_key(j, "exec"); cu_json_str(j, e->exec);
    if (e->icon) { cu_json_key(j, "icon"); cu_json_str(j, e->icon); }
    cu_json_obj_end(j);
  }
  g_list_free(keys);

  g_hash_table_destroy(seen);
}

/* ---------------------------------------------------------------- launching */

/*
 * Collects the WM_CLASS values a desktop entry is known by.
 *
 * This is the missing link between the two worlds: AT-SPI says "kate", the X
 * server says WM_CLASS "kate"/"Kate", and the .desktop file says
 * org.kde.kate — and the only declared relationship between the last two is
 * StartupWMClass, which most entries omit. So in practice the candidates are:
 *
 *   StartupWMClass   when the entry declares it (org.kde.kate -> "kate")
 *   the app id       some apps set it verbatim (org.gnome.Nautilus)
 *   the exec basename "kate" from Exec=kate -b %U
 *
 * Every candidate is lowercased; X11 matching is case-insensitive because apps
 * disagree about capitalising their class ("kate" vs "Kate").
 */
void cu_desktop_wm_class_candidates(const char *query, GPtrArray *out) {
  if (!query || !*query || !out) return;

  GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, entry_free);
  const char *data_home = g_get_user_data_dir();
  const char *data_dirs = g_getenv("XDG_DATA_DIRS");
  if (!data_dirs || !*data_dirs) data_dirs = "/usr/local/share:/usr/share";

  char *user_apps = g_build_filename(data_home, "applications", NULL);
  scan_dir(seen, user_apps);
  g_free(user_apps);
  gchar **dirs = g_strsplit(data_dirs, ":", -1);
  for (int i = 0; dirs[i]; i++) {
    char *apps = g_build_filename(dirs[i], "applications", NULL);
    scan_dir(seen, apps);
    g_free(apps);
  }
  g_strfreev(dirs);

  DesktopEntry *e = NULL;
  /* An installed-app query arrives as the app id or the display name. */
  if (g_hash_table_contains(seen, query)) {
    e = g_hash_table_lookup(seen, query);
  } else {
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, seen);
    while (g_hash_table_iter_next(&it, &k, &v)) {
      DesktopEntry *cand = v;
      if (cand && cand->name && g_ascii_strcasecmp(cand->name, query) == 0) {
        e = cand;
        break;
      }
    }
  }

  if (e && e->wm_class && *e->wm_class) {
    g_ptr_array_add(out, g_ascii_strdown(e->wm_class, -1));
  }
  if (e && e->id && *e->id) {
    g_ptr_array_add(out, g_ascii_strdown(e->id, -1));
  }
  if (e && e->exec && *e->exec) {
    char *prog = g_strdup(e->exec);
    char *space = strchr(prog, ' ');
    if (space) *space = '\0';
    char *base = g_path_get_basename(prog);
    if (base && *base) g_ptr_array_add(out, g_ascii_strdown(base, -1));
    g_free(base);
    g_free(prog);
  }
  /* The query itself: for windows that report a bare executable name as their
     app label, this is the only candidate there is. */
  g_ptr_array_add(out, g_ascii_strdown(query, -1));

  g_hash_table_destroy(seen);
}

/* Finds an installed entry by app id (org.kde.kate), display name, or as a
 * fallback the executable's basename. Returns the Exec= line. */
static char *find_exec_for(const char *query, bool *out_matched_name) {
  if (out_matched_name) *out_matched_name = false;
  if (!query || !*query) return NULL;

  GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, entry_free);
  const char *data_home = g_get_user_data_dir();
  const char *data_dirs = g_getenv("XDG_DATA_DIRS");
  if (!data_dirs || !*data_dirs) data_dirs = "/usr/local/share:/usr/share";

  char *user_apps = g_build_filename(data_home, "applications", NULL);
  scan_dir(seen, user_apps);
  g_free(user_apps);
  gchar **dirs = g_strsplit(data_dirs, ":", -1);
  for (int i = 0; dirs[i]; i++) {
    char *apps = g_build_filename(dirs[i], "applications", NULL);
    scan_dir(seen, apps);
    g_free(apps);
  }
  g_strfreev(dirs);

  char *result = NULL;

  /* 1. exact app id, which is what list_apps advertises as bundleId */
  DesktopEntry *e = g_hash_table_lookup(seen, query);
  if (e && e->exec) result = g_strdup(e->exec);

  /* 2. exact display name, case-insensitive */
  if (!result) {
    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, seen);
    while (g_hash_table_iter_next(&it, &k, &v)) {
      DesktopEntry *cand = v;
      if (cand->exec && cand->name &&
          g_ascii_strcasecmp(cand->name, query) == 0) {
        result = g_strdup(cand->exec);
        if (out_matched_name) *out_matched_name = true;
        break;
      }
    }
  }

  /* 3. an entry whose Exec basename matches, so "kate" finds org.kde.kate */
  if (!result) {
    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, seen);
    while (g_hash_table_iter_next(&it, &k, &v)) {
      DesktopEntry *cand = v;
      if (!cand->exec) continue;
      char *base = g_path_get_basename(cand->exec);
      bool match = g_ascii_strcasecmp(base, query) == 0 ||
                   g_str_has_suffix(cand->id, query);
      g_free(base);
      if (match) { result = g_strdup(cand->exec); break; }
    }
  }

  g_hash_table_destroy(seen);
  return result;
}

/*
 * Runs a command detached from this process.
 *
 * The helper lives as long as the agent session, so an app it spawns must not
 * share its process group: otherwise Ctrl-C in a terminal, or helper shutdown,
 * would take the user's freshly opened app down with it. Double fork is enough
 * here; POSIX_SPAWN_SETSID is not available on glibc, so setsid runs in the
 * child before exec.
 */
static bool spawn_detached(char *const argv[]) {
  pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    /* Intermediate child: detach from the controlling terminal and exit, so the
       grandchild is reparented to init and never becomes a zombie of ours. */
    if (setsid() < 0) _exit(127);
    pid_t pid2 = fork();
    if (pid2 < 0) _exit(127);
    if (pid2 > 0) _exit(0);

    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      if (devnull > 2) close(devnull);
    }
    execvp(argv[0], argv);
    _exit(127);
  }
  int status = 0;
  waitpid(pid, &status, 0);   /* reap only the intermediate child */
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

char *cu_desktop_launch(const char *query, char **resolved_name) {
  if (resolved_name) *resolved_name = NULL;
  if (!query || !*query) return NULL;

  bool matched_name = false;
  char *exec_line = find_exec_for(query, &matched_name);
  if (!exec_line) {
    /* Not an installed entry: allow an absolute path or a bare command, which is
       what a user means when they say "open /usr/bin/htop". */
    if (g_path_is_absolute(query) || g_find_program_in_path(query)) {
      exec_line = g_strdup(query);
    } else {
      return NULL;
    }
  }

  /* The Exec line may carry arguments; split it the way a shell would without
     giving it a shell to interpret. */
  int argc = 0;
  char **argv = NULL;
  GError *err = NULL;
  if (!g_shell_parse_argv(exec_line, &argc, &argv, &err)) {
    if (err) g_error_free(err);
    g_free(exec_line);
    return NULL;
  }
  if (argc == 0 || !argv[0]) {
    g_strfreev(argv);
    g_free(exec_line);
    return NULL;
  }

  /* A desktop entry's Exec is normally the application binary — but it can also
     be an env wrapper or a helper script, and gtk-launch is the officially
     correct entry point when we are launching by app id. Prefer gtk-launch when
     the query looks like an app id so environment/DBus activation matches a real
     menu click. */
  bool launched = false;
  if (strchr(query, '.') != NULL && !g_path_is_absolute(query)) {
    char *gtk_argv[] = { (char *)"gtk-launch", (char *)query, NULL };
    if (g_find_program_in_path("gtk-launch")) launched = spawn_detached(gtk_argv);
  }
  if (!launched) {
    /* Pre-flight: under a double fork an execvp failure in the grandchild is
       invisible to us, so resolve argv[0] here and refuse before we ever
       report a successful launch. */
    const char *bin = argv[0];
    if (!g_path_is_absolute(bin) && !g_find_program_in_path(bin)) {
      g_strfreev(argv);
      g_free(exec_line);
      if (resolved_name) *resolved_name = NULL;
      return NULL;
    }
    launched = spawn_detached(argv);
  }

  if (!launched) {
    g_strfreev(argv);
    g_free(exec_line);
    if (resolved_name) *resolved_name = NULL;
    return NULL;
  }

  if (resolved_name) *resolved_name = g_strdup(query);

  /* Post-launch probe. On an X11/XWayland session a managed window appears in
     the EWMH list after a short delay; we look it up by WM_CLASS and report the
     handle we actually matched. On a pure Wayland session there is no list the
     helper can read, so we only confirm the launch request was issued and do not
     claim the window became visible. */
  char *reply = NULL;
  if (cu_x11_available()) {
    usleep(350000);
    unsigned long win = cu_x11_find_window(0, NULL, query);
    if (win) {
      int pid = 0;
      char *title = NULL;
      cu_x11_window_identity(win, &pid, &title);
      reply = g_strdup_printf("Application launched (x11=%lu%s%s).", win,
                              title ? ", " : "", title ? title : "");
      g_free(title);
    } else {
      reply = g_strdup("Application launch request sent; the window had not "
                       "appeared in the window list yet.");
    }
  } else {
    reply = g_strdup("Application launch request sent; window visibility "
                     "cannot be confirmed on this Wayland session.");
  }

  g_strfreev(argv);
  g_free(exec_line);
  return reply;
}

/* Pure resolution: reports whether `query` names something launchable, without
   starting it. Used by resolve_app so a dry run has no side effects. */
char *cu_desktop_resolve(const char *query) {
  if (!query || !*query) return NULL;
  bool matched_name = false;
  char *exec_line = find_exec_for(query, &matched_name);
  if (exec_line) return exec_line;
  if (g_path_is_absolute(query) || g_find_program_in_path(query)) {
    return g_strdup(query);
  }
  return NULL;
}

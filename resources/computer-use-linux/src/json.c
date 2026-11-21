#include "cu.h"

#include <math.h>
#include <string.h>

/*
 * A tiny streaming JSON writer. The helper only ever *emits* JSON; input is
 * parsed with GVariant's JSON parser (glib >= 2.80), so there is no need for a
 * full DOM implementation here.
 */

void cu_json_init(CuJson *j) {
  j->buf = g_string_new(NULL);
  j->depth = 0;
  memset(j->first, 0, sizeof(j->first));
  j->first[0] = true;
}

void cu_json_free(CuJson *j) {
  if (j->buf) {
    g_string_free(j->buf, TRUE);
    j->buf = NULL;
  }
}

static void json_separator(CuJson *j) {
  if (j->depth <= 0) return;
  if (!j->first[j->depth]) {
    g_string_append_c(j->buf, ',');
  }
  j->first[j->depth] = false;
}

void cu_json_obj_begin(CuJson *j) {
  json_separator(j);
  g_string_append_c(j->buf, '{');
  j->depth++;
  if (j->depth < 64) j->first[j->depth] = true;
}

void cu_json_obj_end(CuJson *j) {
  if (j->depth > 0) j->depth--;
  g_string_append_c(j->buf, '}');
  /* The container that just closed counts as a value in its parent, so the
     next sibling key must be preceded by a comma. */
  if (j->depth < 64) j->first[j->depth] = false;
}

void cu_json_arr_begin(CuJson *j) {
  json_separator(j);
  g_string_append_c(j->buf, '[');
  j->depth++;
  if (j->depth < 64) j->first[j->depth] = true;
}

void cu_json_arr_end(CuJson *j) {
  if (j->depth > 0) j->depth--;
  g_string_append_c(j->buf, ']');
  if (j->depth < 64) j->first[j->depth] = false;
}

static void json_escape_into(GString *out, const char *s) {
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    switch (*p) {
      case '"':  g_string_append(out, "\\\""); break;
      case '\\': g_string_append(out, "\\\\"); break;
      case '\b': g_string_append(out, "\\b");  break;
      case '\f': g_string_append(out, "\\f");  break;
      case '\n': g_string_append(out, "\\n");  break;
      case '\r': g_string_append(out, "\\r");  break;
      case '\t': g_string_append(out, "\\t");  break;
      default:
        if (*p < 0x20) {
          g_string_append_printf(out, "\\u%04x", *p);
        } else {
          g_string_append_c(out, (char)*p);
        }
    }
  }
}

void cu_json_key(CuJson *j, const char *key) {
  json_separator(j);
  g_string_append_c(j->buf, '"');
  json_escape_into(j->buf, key);
  g_string_append(j->buf, "\":");
  /* The value must not emit its own comma, so mark the slot as "already used"
     from the container's point of view while staying in the same depth. */
  j->first[j->depth] = true;
}

/*
 * Keyed containers must run the separator for the *parent* level before opening
 * their own. cu_json_key() marks the current level as satisfied so the value does
 * not introduce a second comma, but the value itself still has to open a fresh
 * level and clear its own "first" flag — otherwise the next sibling key at the
 * parent level would be appended without a comma.
 */
void cu_json_key_obj(CuJson *j, const char *key) {
  json_separator(j);
  g_string_append_c(j->buf, '"');
  json_escape_into(j->buf, key);
  g_string_append(j->buf, "\":{");
  j->depth++;
  if (j->depth < 64) j->first[j->depth] = true;
}

void cu_json_key_arr(CuJson *j, const char *key) {
  json_separator(j);
  g_string_append_c(j->buf, '"');
  json_escape_into(j->buf, key);
  g_string_append(j->buf, "\":[");
  j->depth++;
  if (j->depth < 64) j->first[j->depth] = true;
}

void cu_json_str(CuJson *j, const char *s) {
  json_separator(j);
  if (!s) {
    g_string_append(j->buf, "null");
    return;
  }
  g_string_append_c(j->buf, '"');
  json_escape_into(j->buf, s);
  g_string_append_c(j->buf, '"');
}

void cu_json_int(CuJson *j, long long v) {
  json_separator(j);
  g_string_append_printf(j->buf, "%lld", v);
}

void cu_json_double(CuJson *j, double v) {
  json_separator(j);
  if (!isfinite(v)) {
    g_string_append(j->buf, "null");
    return;
  }
  /* Coordinates are integral in practice; keep integers terse. */
  if (v == (double)(long long)v) {
    g_string_append_printf(j->buf, "%lld", (long long)v);
  } else {
    g_string_append_printf(j->buf, "%g", v);
  }
}

void cu_json_bool(CuJson *j, bool v) {
  json_separator(j);
  g_string_append(j->buf, v ? "true" : "false");
}

void cu_json_null(CuJson *j) {
  json_separator(j);
  g_string_append(j->buf, "null");
}
